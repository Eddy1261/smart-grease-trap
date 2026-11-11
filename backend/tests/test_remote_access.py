"""Remote-access security regression tests.

Covers the optional dashboard access token, service-worker cache headers and
the report-download path traversal fix. No real tunnel, Firebase or phone is
involved.
"""
from fastapi.testclient import TestClient

from backend.app.config import settings
from backend.app.main import app
from backend.app.security import ACCESS_COOKIE_NAME

TOKEN = "test-access-token-0123456789"


def _client() -> TestClient:
    return TestClient(app)


def test_local_access_works_when_token_unset(monkeypatch):
    monkeypatch.setattr(settings, "DASHBOARD_ACCESS_TOKEN", "")
    assert _client().get("/api/status").status_code == 200


def test_public_traffic_is_blocked_when_token_unset(monkeypatch):
    monkeypatch.setattr(settings, "DASHBOARD_ACCESS_TOKEN", "")
    res = _client().get("/health", headers={"CF-Connecting-IP": "203.0.113.10"})
    assert res.status_code == 503
    assert "DASHBOARD_ACCESS_TOKEN" in res.text


def test_api_requires_token_when_configured(monkeypatch):
    monkeypatch.setattr(settings, "DASHBOARD_ACCESS_TOKEN", TOKEN)
    res = _client().get("/api/status")
    assert res.status_code == 401
    assert res.json()["detail"] == "Access token required"
    assert "no-store" in res.headers["cache-control"]


def test_html_navigation_gets_login_page(monkeypatch):
    monkeypatch.setattr(settings, "DASHBOARD_ACCESS_TOKEN", TOKEN)
    res = _client().get("/", headers={"Accept": "text/html"})
    assert res.status_code == 401
    assert "/__access/login" in res.text
    assert "no-store" in res.headers["cache-control"]


def test_login_sets_httponly_cookie_and_unlocks_dashboard_and_sw(monkeypatch):
    monkeypatch.setattr(settings, "DASHBOARD_ACCESS_TOKEN", TOKEN)
    client = _client()

    res = client.post(
        "/__access/login",
        data={"token": TOKEN},
        follow_redirects=False,
    )
    assert res.status_code == 303
    set_cookie = res.headers.get("set-cookie", "")
    assert ACCESS_COOKIE_NAME in set_cookie
    assert "httponly" in set_cookie.lower()

    assert client.get("/api/status").status_code == 200

    sw = client.get("/firebase-messaging-sw.js")
    assert sw.status_code == 200
    assert sw.headers["service-worker-allowed"] == "/"
    assert "no-store" in sw.headers["cache-control"]
    assert sw.headers["x-content-type-options"] == "nosniff"


def test_service_worker_requires_token_when_configured(monkeypatch):
    monkeypatch.setattr(settings, "DASHBOARD_ACCESS_TOKEN", TOKEN)
    assert _client().get("/firebase-messaging-sw.js").status_code == 401


def test_authenticated_public_request_passes(monkeypatch):
    monkeypatch.setattr(settings, "DASHBOARD_ACCESS_TOKEN", TOKEN)
    client = _client()
    assert client.post("/__access/login", data={"token": TOKEN}).status_code == 200
    res = client.get(
        "/api/status",
        headers={"CF-Connecting-IP": "203.0.113.10", "CF-Ray": "test"},
    )
    assert res.status_code == 200


def test_wrong_token_is_rejected(monkeypatch):
    monkeypatch.setattr(settings, "DASHBOARD_ACCESS_TOKEN", TOKEN)
    client = _client()
    res = client.post("/__access/login", data={"token": "wrong-token"})
    assert res.status_code == 401
    assert client.get("/api/status").status_code == 401


def test_health_stays_public_when_token_configured(monkeypatch):
    monkeypatch.setattr(settings, "DASHBOARD_ACCESS_TOKEN", TOKEN)
    assert _client().get("/health").status_code == 200


def test_logout_clears_access(monkeypatch):
    monkeypatch.setattr(settings, "DASHBOARD_ACCESS_TOKEN", TOKEN)
    client = _client()
    assert client.post("/__access/login", data={"token": TOKEN}).status_code == 200
    assert client.get("/api/status").status_code == 200

    res = client.get("/__access/logout", follow_redirects=False)
    assert res.status_code == 303
    assert client.get("/api/status").status_code == 401


def test_report_download_rejects_path_traversal(monkeypatch):
    monkeypatch.setattr(settings, "DASHBOARD_ACCESS_TOKEN", "")
    client = _client()
    for path in (
        "/api/reports/download/..%5C..%5C.env",
        "/api/reports/download/..%5C..%5CREADME.md",
        "/api/reports/download/..%2F..%2F.env",
        "/api/reports/download/not-a-report.txt",
    ):
        res = client.get(path)
        assert res.status_code == 404, path
