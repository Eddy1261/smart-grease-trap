"""Access protection for the dashboard/API when exposed through a tunnel.

The deployment is local-first: the backend binds to 127.0.0.1 and is only
reachable from the laptop. When Cloudflare Tunnel forwards public HTTPS traffic,
Cloudflare adds ``CF-Connecting-IP``/``CF-Ray`` headers. This module uses those
headers to distinguish public traffic from local traffic:

    * no DASHBOARD_ACCESS_TOKEN configured -> local access works, public traffic
      is refused (503). The backend can never be exposed by accident.
    * DASHBOARD_ACCESS_TOKEN configured -> a valid cookie is required for every
      route except ``/health`` and the login/logout endpoints.

The token is entered once through a small login form and stored in an HttpOnly
cookie. No secret is hard-coded and the token is never put in a URL (so it does
not leak into access logs or browser history).
"""
import hashlib
import hmac
import logging

from fastapi import FastAPI, Request
from starlette.middleware.base import BaseHTTPMiddleware
from starlette.responses import HTMLResponse, JSONResponse, RedirectResponse, Response

from backend.app.config import settings

logger = logging.getLogger("smart_grease_trap.security")

ACCESS_COOKIE_NAME = "sgt_access"
LOGIN_PATH = "/__access/login"
LOGOUT_PATH = "/__access/logout"
PUBLIC_PATHS = {"/health"}


def _digest(value: str) -> str:
    return hashlib.sha256(value.encode("utf-8")).hexdigest()


def _configured_token() -> str:
    return (settings.DASHBOARD_ACCESS_TOKEN or "").strip()


def _is_cloudflare_request(request: Request) -> bool:
    return bool(
        request.headers.get("cf-connecting-ip")
        or request.headers.get("cf-ray")
    )


def _is_https(request: Request) -> bool:
    forwarded = request.headers.get("x-forwarded-proto", "")
    scheme = forwarded.split(",")[0].strip() if forwarded else request.url.scheme
    return scheme.lower() == "https"


def _wants_html(request: Request) -> bool:
    return request.method == "GET" and "text/html" in request.headers.get("accept", "")


def _no_store_headers() -> dict:
    return {
        "Cache-Control": "no-store, no-cache, must-revalidate",
        "Pragma": "no-cache",
        "X-Frame-Options": "DENY",
        "Referrer-Policy": "no-referrer",
    }


_LOGIN_PAGE = """<!DOCTYPE html>
<html lang="id">
<head>
  <meta charset="UTF-8">
  <meta name="viewport" content="width=device-width, initial-scale=1.0">
  <title>Smart Grease Trap - Akses</title>
  <style>
    :root { color-scheme: dark; }
    * { box-sizing: border-box; }
    body {
      margin: 0; min-height: 100vh; display: flex; align-items: center;
      justify-content: center; background: #0f172a; color: #e2e8f0;
      font-family: system-ui, -apple-system, Segoe UI, Roboto, sans-serif;
      padding: 24px;
    }
    .card {
      width: 100%; max-width: 380px; background: #1e293b;
      border: 1px solid #334155; border-radius: 14px; padding: 28px;
      box-shadow: 0 20px 40px rgba(0, 0, 0, .35);
    }
    .logo {
      width: 42px; height: 42px; border-radius: 10px; background: #0891b2;
      display: flex; align-items: center; justify-content: center;
      font-weight: 700; margin-bottom: 16px;
    }
    h1 { font-size: 18px; margin: 0 0 6px; }
    p { font-size: 13px; line-height: 1.5; color: #94a3b8; margin: 0 0 18px; }
    label { display: block; font-size: 12px; color: #cbd5e1; margin-bottom: 6px; }
    input {
      width: 100%; padding: 11px 12px; border-radius: 8px;
      border: 1px solid #475569; background: #0f172a; color: #f8fafc;
      font-size: 14px; outline: none;
    }
    input:focus { border-color: #06b6d4; }
    button {
      width: 100%; margin-top: 14px; padding: 11px 12px; border: 0;
      border-radius: 8px; background: #0891b2; color: white;
      font-size: 14px; font-weight: 600; cursor: pointer;
    }
    button:hover { background: #06b6d4; }
    .err {
      margin: 0 0 14px; padding: 10px 12px; border-radius: 8px;
      background: rgba(244, 63, 94, .12); border: 1px solid rgba(244, 63, 94, .45);
      color: #fda4af; font-size: 12px;
    }
    .hint { margin: 16px 0 0; font-size: 11px; color: #64748b; }
  </style>
</head>
<body>
  <main class="card">
    <div class="logo">SGT</div>
    <h1>Smart Grease Trap</h1>
    <p>Masukkan token akses untuk membuka dashboard.</p>
    <!--ERROR-->
    <form method="post" action="/__access/login">
      <label for="token">Token akses</label>
      <input id="token" name="token" type="password" required autofocus
             autocomplete="current-password" autocapitalize="off"
             spellcheck="false" placeholder="Tempel token dari .env laptop">
      <button type="submit">Masuk</button>
    </form>
    <p class="hint">Token disimpan di <code>DASHBOARD_ACCESS_TOKEN</code> pada
      <code>.env</code> laptop. Jangan dibagikan.</p>
  </main>
</body>
</html>
"""

_ERROR_BLOCK = '<p class="err">Token salah. Periksa kembali.</p>'


def _login_response(error: bool = False, status_code: int = 401) -> HTMLResponse:
    html = _LOGIN_PAGE.replace("<!--ERROR-->", _ERROR_BLOCK if error else "")
    return HTMLResponse(content=html, status_code=status_code, headers=_no_store_headers())


def _blocked_response(request: Request, message: str) -> Response:
    if _wants_html(request):
        html = (
            "<!DOCTYPE html><html lang='id'><head><meta charset='UTF-8'>"
            "<meta name='viewport' content='width=device-width, initial-scale=1.0'>"
            "<title>Smart Grease Trap - Akses dinonaktifkan</title></head>"
            "<body style='font-family:system-ui,sans-serif;background:#0f172a;"
            "color:#e2e8f0;padding:32px;line-height:1.6'>"
            "<h1 style='font-size:18px'>Akses publik dinonaktifkan</h1>"
            f"<p style='color:#94a3b8;font-size:13px'>{message}</p>"
            "</body></html>"
        )
        return HTMLResponse(content=html, status_code=503, headers=_no_store_headers())
    return JSONResponse(
        {"detail": message},
        status_code=503,
        headers=_no_store_headers(),
    )


class AccessTokenMiddleware(BaseHTTPMiddleware):
    """Enforce the dashboard access token for public (tunnel) traffic."""

    async def dispatch(self, request: Request, call_next):
        token = _configured_token()
        is_public = _is_cloudflare_request(request)

        if not token:
            if is_public:
                return _blocked_response(
                    request,
                    "DASHBOARD_ACCESS_TOKEN belum dikonfigurasi di .env backend. "
                    "Set token lalu restart backend sebelum membuka akses publik.",
                )
            return await call_next(request)

        if request.url.path in PUBLIC_PATHS or request.url.path in (LOGIN_PATH, LOGOUT_PATH):
            return await call_next(request)

        cookie = request.cookies.get(ACCESS_COOKIE_NAME, "")
        if cookie and hmac.compare_digest(cookie, _digest(token)):
            return await call_next(request)

        if _wants_html(request):
            return _login_response()
        return JSONResponse(
            {"detail": "Access token required"},
            status_code=401,
            headers=_no_store_headers(),
        )


async def access_login(request: Request) -> Response:
    token = _configured_token()
    if not token:
        return _blocked_response(
            request,
            "DASHBOARD_ACCESS_TOKEN belum dikonfigurasi di .env backend.",
        )

    try:
        form = await request.form()
    except Exception:  # malformed body -> show the form again, never a 500
        return _login_response(error=True, status_code=400)

    raw = form.get("token")
    submitted = raw.strip() if isinstance(raw, str) else ""

    if submitted and hmac.compare_digest(_digest(submitted), _digest(token)):
        response = RedirectResponse("/", status_code=303)
        response.set_cookie(
            key=ACCESS_COOKIE_NAME,
            value=_digest(token),
            max_age=max(0, settings.ACCESS_COOKIE_MAX_AGE),
            httponly=True,
            samesite="lax",
            secure=_is_https(request),
            path="/",
        )
        return response

    client_ip = request.headers.get("cf-connecting-ip") or (
        request.client.host if request.client else "unknown"
    )
    logger.warning("Rejected dashboard access token from %s", client_ip)
    return _login_response(error=True)


async def access_logout(request: Request) -> Response:
    response = RedirectResponse("/", status_code=303)
    response.delete_cookie(ACCESS_COOKIE_NAME, path="/")
    return response


def register_access_routes(app: FastAPI) -> None:
    app.add_api_route(LOGIN_PATH, access_login, methods=["POST"], include_in_schema=False)
    app.add_api_route(LOGOUT_PATH, access_logout, methods=["GET", "POST"], include_in_schema=False)
