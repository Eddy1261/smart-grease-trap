"""FCM notification service (Step 9 — Halim Master Audit, §24).

Real Firebase Admin SDK integration with:
    - graceful stub mode when credentials/SDK are unavailable
    - event mapping from backend status transitions
    - deduplication via transition detection + per-event cooldown
    - error handling that never crashes the telemetry pipeline
    - injectable sender for tests (no cloud calls in tests)

Credentials policy:
    - service-account JSON path comes from FIREBASE_CREDENTIALS_PATH
      (outside the repository, never committed)
    - web config / VAPID key come from environment variables
    - no secrets are ever hard-coded here
"""
import logging
import time
from dataclasses import dataclass, field
from pathlib import Path
from typing import Any, Callable, Dict, List, Optional

logger = logging.getLogger("smart_grease_trap.notifications")

# Backend status -> notification event mapping (audit §24 candidate events)
STATUS_EVENT_MAP: Dict[str, Dict[str, str]] = {
    "GANGGUAN_SENSOR": {
        "event_type": "SENSOR_FAULT",
        "title": "Smart Grease Trap: sensor fault",
        "body": "Device {device_id} entered GANGGUAN_SENSOR. Solenoid fail-safe CLOSED.",
    },
    "PENUH": {
        "event_type": "CONTAINER_NEAR_FULL",
        "title": "Smart Grease Trap: collector near full",
        "body": "Device {device_id} collector is near capacity. Schedule emptying.",
    },
    "EMULSI_TERDETEKSI": {
        "event_type": "CRITICAL_OUTLET",
        "title": "Smart Grease Trap: critical outlet condition",
        "body": "Device {device_id} reports emulsified outlet water (EMULSI_TERDETEKSI).",
    },
    "SKIMMING_AKTIF": {
        "event_type": "SKIMMING_STARTED",
        "title": "Smart Grease Trap: skimming active",
        "body": "Device {device_id} started an adaptive skimming cycle.",
    },
}


@dataclass
class DeliveryResult:
    attempted: int = 0
    success: int = 0
    failed: int = 0
    stub: bool = False
    errors: List[str] = field(default_factory=list)

    def as_dict(self) -> Dict[str, Any]:
        return {
            "attempted": self.attempted,
            "success": self.success,
            "failed": self.failed,
            "stub": self.stub,
            "errors": self.errors,
        }


def build_web_config() -> Optional[Dict[str, str]]:
    """Firebase Web App config from environment variables (public identifiers)."""
    from backend.app.config import settings

    required = {
        "apiKey": settings.FIREBASE_WEB_API_KEY,
        "authDomain": settings.FIREBASE_WEB_AUTH_DOMAIN,
        "projectId": settings.FIREBASE_WEB_PROJECT_ID,
        "storageBucket": settings.FIREBASE_WEB_STORAGE_BUCKET,
        "messagingSenderId": settings.FIREBASE_WEB_MESSAGING_SENDER_ID,
        "appId": settings.FIREBASE_WEB_APP_ID,
    }
    if not all(required.values()):
        return None
    if settings.FIREBASE_WEB_MEASUREMENT_ID:
        required["measurementId"] = settings.FIREBASE_WEB_MEASUREMENT_ID
    return required


class FCMNotificationService:
    def __init__(
        self,
        db=None,
        credentials_path: Optional[str] = None,
        cooldown_seconds: Optional[float] = None,
        now_fn: Optional[Callable[[], float]] = None,
        sender: Optional[Callable[[str, str, str, Dict[str, str]], None]] = None,
        enabled: Optional[bool] = None,
    ):
        """`sender` is injectable for tests: sender(token, title, body, data),
        raising on failure. `enabled` overrides environment auto-detection."""
        self.db = db
        self._credentials_path_override = credentials_path
        self._enabled_override = enabled
        self._now = now_fn or time.time
        self._sender = sender
        self.outbox: List[Dict[str, Any]] = []
        self._last_sent: Dict[tuple, float] = {}
        self._last_status: Dict[str, str] = {}
        self._last_maintenance: Dict[str, bool] = {}

        if cooldown_seconds is None:
            from backend.app.config import settings
            cooldown_seconds = settings.FCM_DEDUP_COOLDOWN_SECONDS
        self._cooldown = cooldown_seconds

    # ------------------------------------------------------------------ config
    def _credentials_path(self) -> str:
        if self._credentials_path_override is not None:
            return self._credentials_path_override
        from backend.app.config import settings
        return settings.FIREBASE_CREDENTIALS_PATH

    def server_delivery_available(self) -> bool:
        """True when the backend can actually deliver via Firebase Admin SDK."""
        if self._enabled_override is not None:
            return self._enabled_override
        from backend.app.config import settings
        if settings.FCM_ENABLED.lower() == "false":
            return False
        if self._sender is not None:
            return True
        path = self._credentials_path()
        if not path or not Path(path).exists():
            return False
        try:
            import firebase_admin  # noqa: F401
            return True
        except ImportError:
            return False

    def client_config_available(self) -> bool:
        """True when the dashboard can request an FCM token (web config + VAPID)."""
        from backend.app.config import settings
        return build_web_config() is not None and bool(settings.FCM_VAPID_KEY)

    def _ensure_firebase_app(self):
        import firebase_admin
        from firebase_admin import credentials

        if not firebase_admin._apps:
            cred = credentials.Certificate(self._credentials_path())
            firebase_admin.initialize_app(cred)
        return firebase_admin.get_app()

    # -------------------------------------------------------------------- send
    def send_to_tokens(
        self,
        tokens: List[str],
        title: str,
        body: str,
        data: Optional[Dict[str, Any]] = None,
    ) -> DeliveryResult:
        result = DeliveryResult()
        if not tokens:
            return result

        payload = {k: str(v) for k, v in (data or {}).items()}

        if not self.server_delivery_available():
            # Stub mode: record to outbox + event log; never claim real delivery.
            result.stub = True
            for token in tokens:
                result.attempted += 1
                self.outbox.append({
                    "token": token, "title": title, "body": body,
                    "data": payload, "at": self._now(),
                })
            self._log_event("NOTIFICATION_STUB", "INFO",
                            f"Stub notification '{title}' to {len(tokens)} token(s)")
            return result

        if self._sender is not None:
            for token in tokens:
                result.attempted += 1
                try:
                    self._sender(token, title, body, payload)
                    result.success += 1
                except Exception as exc:  # noqa: BLE001 — never crash the pipeline
                    result.failed += 1
                    result.errors.append(f"{token[:12]}...: {exc}")
                    logger.warning("FCM stub-sender failure: %s", exc)
            return result

        try:
            self._ensure_firebase_app()
            from firebase_admin import messaging
        except Exception as exc:  # credentials invalid / SDK issue
            result.failed = len(tokens)
            result.errors.append(f"firebase init failed: {exc}")
            self._log_event("NOTIFICATION_INIT_FAILED", "ERROR", str(exc))
            return result

        for token in tokens:
            result.attempted += 1
            try:
                message = messaging.Message(
                    notification=messaging.Notification(title=title, body=body),
                    data=payload,
                    token=token,
                )
                messaging.send(message)
                result.success += 1
            except Exception as exc:  # noqa: BLE001 — per-token isolation
                result.failed += 1
                result.errors.append(f"{token[:12]}...: {exc}")
                logger.warning("FCM delivery failure for token %s...: %s", token[:12], exc)
                # Drop permanently invalid tokens to prevent repeated failures
                if self.db is not None and _is_invalid_token_error(exc):
                    try:
                        self.db.remove_notification_token(token)
                    except Exception:  # noqa: BLE001
                        pass
        self._log_event(
            "NOTIFICATION_SENT",
            "INFO" if result.failed == 0 else "WARNING",
            f"'{title}': {result.success}/{result.attempted} delivered",
        )
        return result

    # ------------------------------------------------------------ dedup logic
    def notify_event(
        self,
        device_id: str,
        event_type: str,
        title: str,
        body: str,
        data: Optional[Dict[str, Any]] = None,
        cooldown_key: Optional[str] = None,
    ) -> Optional[DeliveryResult]:
        """Send unless the same event fired within the cooldown window."""
        key = (device_id, cooldown_key or event_type)
        now = self._now()
        last = self._last_sent.get(key)
        if last is not None and (now - last) < self._cooldown:
            logger.info("Notification suppressed (cooldown): %s for %s", event_type, device_id)
            return None
        self._last_sent[key] = now

        tokens: List[str] = []
        if self.db is not None:
            tokens = [row["token"] for row in self.db.get_notification_tokens()]
        return self.send_to_tokens(tokens, title, body, data)

    def handle_telemetry(self, telemetry, status) -> Optional[DeliveryResult]:
        """Map a classified telemetry message to a deduplicated notification."""
        device_id = telemetry.device_id
        status_value = status.value if hasattr(status, "value") else str(status)

        # One-shot maintenance condition (process did not reach safe criteria).
        # Rising-edge detection + cooldown => exactly one notification per event.
        maintenance = bool(getattr(telemetry.system, "maintenance_required", False))
        previous_maintenance = self._last_maintenance.get(device_id, False)
        self._last_maintenance[device_id] = maintenance
        if maintenance and not previous_maintenance:
            # This cycle's notification duty is fulfilled by the maintenance
            # alert: record the status so it is not re-notified immediately.
            self._last_status[device_id] = status_value
            return self.notify_event(
                device_id=device_id,
                event_type="MAINTENANCE_REQUIRED",
                title="Smart Grease Trap: outlet quality not reached",
                body=(
                    f"Device {device_id} did not reach the safe outlet criteria within the "
                    "configured timeout. The valve was kept CLOSED; user inspection is recommended."
                ),
                data={"device_id": device_id, "event_type": "MAINTENANCE_REQUIRED", "status": status_value},
            )

        previous = self._last_status.get(device_id)
        self._last_status[device_id] = status_value
        if previous == status_value:
            return None

        mapping = STATUS_EVENT_MAP.get(status_value)
        if mapping is None:
            return None

        return self.notify_event(
            device_id=device_id,
            event_type=mapping["event_type"],
            title=mapping["title"],
            body=mapping["body"].format(device_id=device_id),
            data={"status": status_value, "device_id": device_id, "event_type": mapping["event_type"]},
        )

    def _log_event(self, event_type: str, severity: str, description: str) -> None:
        if self.db is None:
            return
        try:
            self.db.log_event(event_type, severity, description)
        except Exception as exc:  # noqa: BLE001
            logger.warning("Could not log notification event: %s", exc)


def _is_invalid_token_error(exc: Exception) -> bool:
    text = str(exc).lower()
    return "not a valid fcm registration token" in text or "unregistered" in text or "invalid registration" in text
