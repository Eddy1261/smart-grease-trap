from pathlib import Path
from pydantic_settings import BaseSettings, SettingsConfigDict
from pydantic import Field

BASE_DIR = Path(__file__).resolve().parent.parent.parent

class Settings(BaseSettings):
    # MQTT Configuration
    MQTT_BROKER_HOST: str = "localhost"
    MQTT_BROKER_PORT: int = 1883
    MQTT_TELEMETRY_TOPIC: str = "smart_grease_trap/telemetry"
    MQTT_COMMAND_TOPIC: str = "smart_grease_trap/command"
    MQTT_CLIENT_ID: str = "sgt_backend_service"
    MQTT_USERNAME: str = ""
    MQTT_PASSWORD: str = ""
    MQTT_KEEP_ALIVE: int = 60

    # Persistence
    DATABASE_URL: str = "sqlite:///./data/smart_grease_trap.db"

    # API Configuration
    API_HOST: str = "127.0.0.1"
    API_PORT: int = 8000
    API_DEBUG: bool = False
    LOG_LEVEL: str = "INFO"

    # Remote access protection (Cloudflare Tunnel / public HTTPS).
    # Empty = local-only mode: requests carrying Cloudflare headers are refused,
    # so the backend can never be exposed publicly without an explicit token.
    DASHBOARD_ACCESS_TOKEN: str = ""
    ACCESS_COOKIE_MAX_AGE: int = 2592000  # 30 days, seconds
    # Comma-separated list of allowed cross-origin dashboard origins.
    # Empty = no CORS middleware (dashboard and API are same-origin).
    CORS_ALLOW_ORIGINS: str = ""

    # System Simulation & Calibration Defaults
    DEFAULT_DEVICE_ID: str = "SGT-001"
    DEFAULT_SURFACE_AREA_CM2: float = 1800.0
    DEFAULT_ALPHA_CORR: float = 1.0
    DEFAULT_EMA_ALPHA: float = 0.20
    DEFAULT_CRITICAL_VOLUME_ML: float = 7200.0
    DEFAULT_DEBOUNCE_CYCLES: int = 3
    DEFAULT_SAMPLING_INTERVAL_S: float = 30.0

    # Backend Classifier Thresholds
    THRESHOLD_TURBIDITY_EMULSION_NTU: float = 150.0
    THRESHOLD_OIL_CONCENTRATION_MG_L: float = 10.0
    THRESHOLD_CONTAINER_MAX_MASS_G: float = 4000.0
    THRESHOLD_CONTAINER_NEAR_FULL_G: float = 4000.0

    # Firebase / FCM Notifications
    # Path to the service-account JSON, OUTSIDE the repository. Never commit it.
    FIREBASE_CREDENTIALS_PATH: str = ""
    # auto | true | false — auto enables delivery when credentials + firebase-admin are present
    FCM_ENABLED: str = "auto"
    # Public VAPID key (Firebase Console -> Cloud Messaging -> Web Push certificates)
    FCM_VAPID_KEY: str = ""
    # Firebase Web App configuration (public client identifiers)
    FIREBASE_WEB_API_KEY: str = ""
    FIREBASE_WEB_AUTH_DOMAIN: str = ""
    FIREBASE_WEB_PROJECT_ID: str = ""
    FIREBASE_WEB_STORAGE_BUCKET: str = ""
    FIREBASE_WEB_MESSAGING_SENDER_ID: str = ""
    FIREBASE_WEB_APP_ID: str = ""
    FIREBASE_WEB_MEASUREMENT_ID: str = ""
    # Dedup cooldown for repeated notification events (seconds)
    FCM_DEDUP_COOLDOWN_SECONDS: float = 300.0

    # File paths
    REPORTS_DIR: str = str(BASE_DIR / "data" / "reports")
    DASHBOARD_STATIC_DIR: str = str(BASE_DIR / "dashboard")

    model_config = SettingsConfigDict(
        env_file=str(BASE_DIR / ".env"),
        env_file_encoding="utf-8",
        extra="ignore"
    )

settings = Settings()
