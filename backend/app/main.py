import json
import logging
from pathlib import Path
from contextlib import asynccontextmanager
from fastapi import FastAPI
from fastapi.middleware.cors import CORSMiddleware
from fastapi.responses import Response
from fastapi.staticfiles import StaticFiles

from backend.app.config import settings
from backend.app.api.routes import router
from backend.app.mqtt.subscriber import MQTTSubscriberService
from backend.app.notifications.fcm_service import FCMNotificationService, build_web_config
from backend.app.security import AccessTokenMiddleware, register_access_routes

logging.basicConfig(
    level=getattr(logging, settings.LOG_LEVEL.upper(), logging.INFO),
    format="%(asctime)s [%(levelname)s] %(name)s: %(message)s"
)
logger = logging.getLogger("smart_grease_trap.main")

if not (settings.DASHBOARD_ACCESS_TOKEN or "").strip():
    logger.warning(
        "DASHBOARD_ACCESS_TOKEN is empty: local access only. "
        "Cloudflare tunnel requests will be refused with HTTP 503 until it is set."
    )
else:
    logger.info("Dashboard access-token protection is enabled for public traffic.")

mqtt_subscriber = MQTTSubscriberService()
notification_service = FCMNotificationService(db=mqtt_subscriber.db)

@asynccontextmanager
async def lifespan(app: FastAPI):
    logger.info("Initializing Smart Grease Trap Backend Service...")
    # Notification mapping on classified telemetry (dedup + stub-safe)
    mqtt_subscriber.set_on_message_callback(
        lambda telemetry, status: notification_service.handle_telemetry(telemetry, status)
    )
    # Start MQTT background subscriber
    mqtt_subscriber.start()
    yield
    logger.info("Shutting down Smart Grease Trap Backend Service...")
    mqtt_subscriber.stop()

app = FastAPI(
    title="Smart Grease Trap Backend Service",
    description="Backend API and Telemetry Pipeline for Smart Grease Trap (Capstone C-251)",
    version="0.1.0",
    lifespan=lifespan
)
app.state.mqtt_subscriber = mqtt_subscriber

# Access protection for public tunnel traffic (no-op for local-only requests
# when DASHBOARD_ACCESS_TOKEN is empty).
app.add_middleware(AccessTokenMiddleware)

# Optional CORS allowlist for separate dev origins. The dashboard is served by
# this same app, so no CORS entry is needed in normal (local or tunnel) use.
# Wildcards are deliberately not used with credentials.
_cors_origins = [o.strip() for o in settings.CORS_ALLOW_ORIGINS.split(",") if o.strip()]
if "*" in _cors_origins:
    logger.warning("CORS_ALLOW_ORIGINS contains '*'; ignoring wildcard because credentials are enabled.")
    _cors_origins = [o for o in _cors_origins if o != "*"]
if _cors_origins:
    app.add_middleware(
        CORSMiddleware,
        allow_origins=_cors_origins,
        allow_credentials=True,
        allow_methods=["*"],
        allow_headers=["*"],
    )

# Include API Router
app.include_router(router)

# Access-token login/logout endpoints (must be registered before the static mount)
register_access_routes(app)

# Firebase Messaging service worker (config injected from environment variables).
# Served at the root scope so it can control the whole dashboard origin.
@app.get("/firebase-messaging-sw.js", include_in_schema=False)
def firebase_messaging_service_worker():
    web_config = build_web_config()
    config_js = json.dumps(web_config) if web_config else "null"
    js = f"""/* Smart Grease Trap FCM service worker (config injected by backend) */
importScripts('https://www.gstatic.com/firebasejs/10.12.2/firebase-app-compat.js');
importScripts('https://www.gstatic.com/firebasejs/10.12.2/firebase-messaging-compat.js');
const firebaseConfig = {config_js};
if (firebaseConfig) {{
  firebase.initializeApp(firebaseConfig);
  const messaging = firebase.messaging();
  messaging.onBackgroundMessage((payload) => {{
    const n = payload.notification || {{}};
    const data = payload.data || {{}};
    self.registration.showNotification(n.title || 'Smart Grease Trap', {{
      body: n.body || '',
      data,
      tag: data.event_type || ('sgt-' + Date.now()),
      renotify: true
    }});
  }});
  self.addEventListener('notificationclick', (event) => {{
    event.notification.close();
    event.waitUntil(
      clients.matchAll({{ type: 'window', includeUncontrolled: true }}).then((list) => {{
        for (const client of list) {{
          if (client.url.startsWith(self.location.origin)) return client.focus();
        }}
        return clients.openWindow('/');
      }})
    );
  }});
}} else {{
  console.warn('FCM web config not configured on the backend; service worker idle.');
}}
"""
    return Response(
        content=js,
        media_type="application/javascript",
        headers={
            # Never let Cloudflare/browser serve a stale service worker with
            # outdated Firebase config.
            "Cache-Control": "no-store, no-cache, must-revalidate",
            "Pragma": "no-cache",
            "Service-Worker-Allowed": "/",
            "X-Content-Type-Options": "nosniff",
        },
    )

# Mount Dashboard static files if directory exists
dashboard_dir = Path(settings.DASHBOARD_STATIC_DIR)
if dashboard_dir.exists():
    app.mount("/", StaticFiles(directory=str(dashboard_dir), html=True), name="dashboard")

if __name__ == "__main__":
    import uvicorn
    uvicorn.run(
        "backend.app.main:app",
        host=settings.API_HOST,
        port=settings.API_PORT,
        reload=settings.API_DEBUG
    )
