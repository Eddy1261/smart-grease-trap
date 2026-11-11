import json
import logging
from typing import Optional, Callable
import paho.mqtt.client as mqtt
from pydantic import ValidationError

from backend.app.config import settings
from backend.app.schemas.telemetry import TelemetryMessage, BackendStatus
from backend.app.classifier.status_classifier import StatusClassifier
from backend.app.db.database import DatabaseManager

logger = logging.getLogger("smart_grease_trap.mqtt")

class MQTTSubscriberService:
    """
    Subscribes to Smart Grease Trap telemetry topics, validates JSON contracts,
    classifies backend operational status, logs state transitions, and persists to SQLite.
    """

    def __init__(
        self,
        broker_host: str = settings.MQTT_BROKER_HOST,
        broker_port: int = settings.MQTT_BROKER_PORT,
        topic: str = settings.MQTT_TELEMETRY_TOPIC,
        db: Optional[DatabaseManager] = None,
        classifier: Optional[StatusClassifier] = None
    ):
        self.broker_host = broker_host
        self.broker_port = broker_port
        self.topic = topic
        self.db = db or DatabaseManager()
        self.classifier = classifier or StatusClassifier()

        self._client: Optional[mqtt.Client] = None
        self._is_connected = False
        self._last_status: Optional[BackendStatus] = None
        self._last_maintenance_required: bool = False
        self._on_message_callback: Optional[Callable[[TelemetryMessage, BackendStatus], None]] = None

    def set_on_message_callback(self, cb: Callable[[TelemetryMessage, BackendStatus], None]):
        self._on_message_callback = cb

    def _on_connect(self, client, userdata, flags, rc):
        if rc == 0:
            self._is_connected = True
            logger.info(f"Connected to MQTT broker at {self.broker_host}:{self.broker_port}")
            client.subscribe(self.topic, qos=1)
            logger.info(f"Subscribed to topic: {self.topic}")
            self.db.log_event("MQTT_CONNECTED", "INFO", f"Connected to broker {self.broker_host}:{self.broker_port}")
        else:
            self._is_connected = False
            logger.error(f"Failed to connect to MQTT broker, return code: {rc}")
            self.db.log_event("MQTT_CONNECT_FAILED", "ERROR", f"Connection failed with code {rc}")

    def _on_disconnect(self, client, userdata, rc):
        self._is_connected = False
        logger.warning(f"Disconnected from MQTT broker (code: {rc})")
        self.db.log_event("MQTT_DISCONNECTED", "WARNING", f"Disconnected with code {rc}")

    def _on_message(self, client, userdata, msg):
        try:
            payload_str = msg.payload.decode("utf-8")
            data = json.loads(payload_str)
            telemetry = TelemetryMessage.model_validate(data)
        except (json.JSONDecodeError, UnicodeDecodeError) as e:
            logger.error(f"Malformed JSON on topic {msg.topic}: {e}")
            self.db.log_event(
                "MALFORMED_TELEMETRY_PAYLOAD", "ERROR",
                f"JSON decode failed: {str(e)}", metadata={"raw_payload": str(msg.payload)[:200]}
            )
            return
        except ValidationError as e:
            logger.error(f"Schema validation failed on topic {msg.topic}: {e}")
            self.db.log_event(
                "SCHEMA_VALIDATION_ERROR", "ERROR",
                f"Validation error: {e.error_count()} errors", metadata={"errors": e.errors()[:3]}
            )
            return

        # Classify status
        current_status = self.classifier.classify(telemetry)

        # One-shot maintenance condition (rising edge only — no per-cycle spam)
        maintenance = bool(getattr(telemetry.system, "maintenance_required", False))
        if maintenance and not self._last_maintenance_required:
            logger.warning(f"Maintenance required for {telemetry.device_id}: outlet quality not reached")
            self.db.log_event(
                "MAINTENANCE_REQUIRED",
                "WARNING",
                "Outlet did not reach the safe criteria within the configured timeout; "
                "valve kept CLOSED. User inspection recommended.",
                device_id=telemetry.device_id,
            )
        self._last_maintenance_required = maintenance

        # Detect and log state transitions
        if self._last_status != current_status:
            logger.info(f"Status transition: {self._last_status} -> {current_status}")
            self.db.log_event(
                "STATUS_TRANSITION",
                "INFO" if current_status == BackendStatus.NORMAL else "WARNING",
                f"Status transitioned from {self._last_status} to {current_status.value}",
                device_id=telemetry.device_id,
                metadata={"from": self._last_status.value if self._last_status else None, "to": current_status.value}
            )
            self._last_status = current_status

        # Persist to database
        self.db.save_telemetry(telemetry, current_status)

        # Optional user callback (e.g. for WebSocket or test hooks)
        if self._on_message_callback:
            try:
                self._on_message_callback(telemetry, current_status)
            except Exception as e:
                logger.error(f"Error in on_message callback: {e}")

    def start(self):
        logger.info(f"Starting MQTT Subscriber client for {self.broker_host}:{self.broker_port}...")
        self._client = mqtt.Client(client_id=settings.MQTT_CLIENT_ID, clean_session=True)
        if settings.MQTT_USERNAME and settings.MQTT_PASSWORD:
            self._client.username_pw_set(settings.MQTT_USERNAME, settings.MQTT_PASSWORD)

        self._client.on_connect = self._on_connect
        self._client.on_disconnect = self._on_disconnect
        self._client.on_message = self._on_message

        try:
            self._client.connect_async(self.broker_host, self.broker_port, keepalive=settings.MQTT_KEEP_ALIVE)
            self._client.loop_start()
        except Exception as e:
            logger.warning(f"Could not connect to MQTT broker at {self.broker_host}:{self.broker_port}: {e}")

    def publish_command(self, command: dict) -> bool:
        """Publish a dashboard command to the firmware command topic.

        Returns False when the broker link is down or the publish call fails,
        so the API can report 503 instead of pretending the command was sent.
        """
        if not self._client or not self._is_connected:
            return False
        try:
            payload = json.dumps(command)
            info = self._client.publish(
                settings.MQTT_COMMAND_TOPIC, payload, qos=1, retain=False
            )
            if info.rc != mqtt.MQTT_ERR_SUCCESS:
                logger.error(
                    f"Command publish failed on {settings.MQTT_COMMAND_TOPIC}: rc={info.rc}"
                )
                return False
            return True
        except Exception as e:
            logger.error(f"Command publish error: {e}")
            return False

    def stop(self):
        if self._client:
            logger.info("Stopping MQTT Subscriber loop...")
            self._client.loop_stop()
            self._client.disconnect()
            self._client = None
            self._is_connected = False
