import sqlite3
import json
from pathlib import Path
from typing import List, Optional, Dict, Any
from datetime import datetime, timezone
from backend.app.schemas.telemetry import TelemetryMessage, BackendStatus
from backend.app.config import settings


def _as_utc_iso(value: datetime) -> str:
    """Normalize a datetime to an ISO-8601 UTC string for SQL comparison."""
    if value.tzinfo is None:
        value = value.replace(tzinfo=timezone.utc)
    return value.astimezone(timezone.utc).isoformat()


class DatabaseManager:
    """
    SQLite persistence abstraction for local laptop development and fallback.
    Provides schema migrations, thread-safe connection handling, and structured queries.
    """

    def __init__(self, db_url: str = settings.DATABASE_URL):
        self.db_path = self._parse_db_path(db_url)
        self._memory_conn: Optional[sqlite3.Connection] = None
        if self.db_path != ":memory:":
            Path(self.db_path).parent.mkdir(parents=True, exist_ok=True)
        else:
            self._memory_conn = sqlite3.connect(":memory:", check_same_thread=False)
            self._memory_conn.row_factory = sqlite3.Row
        self.init_schema()

    def _parse_db_path(self, db_url: str) -> str:
        if db_url.startswith("sqlite:///"):
            return db_url.replace("sqlite:///", "")
        return db_url

    def get_connection(self) -> sqlite3.Connection:
        if self._memory_conn is not None:
            return self._memory_conn
        conn = sqlite3.connect(self.db_path, check_same_thread=False)
        conn.row_factory = sqlite3.Row
        return conn

    def init_schema(self):
        with self.get_connection() as conn:
            cursor = conn.cursor()
            # Telemetry Table
            cursor.execute("""
            CREATE TABLE IF NOT EXISTS telemetry (
                id INTEGER PRIMARY KEY AUTOINCREMENT,
                device_id TEXT NOT NULL,
                timestamp TEXT NOT NULL,
                schema_version INTEGER NOT NULL,
                ultrasonic_raw REAL,
                ultrasonic_median REAL,
                ultrasonic_filtered REAL,
                turbidity_raw REAL,
                turbidity_ntu REAL,
                oil_estimated_mg_l REAL,
                loadcell_raw REAL,
                oil_mass_g REAL,
                surface_height_cm REAL,
                accumulated_volume_ml REAL,
                critical_volume_ml REAL,
                motor_pwm INTEGER,
                motor_state TEXT,
                heater_state TEXT,
                solenoid_state TEXT,
                embedded_state TEXT,
                backend_status TEXT,
                sensor_fault INTEGER,
                safety_state TEXT,
                wifi_connected INTEGER,
                mqtt_connected INTEGER,
                raw_payload TEXT NOT NULL,
                created_at TEXT NOT NULL
            );
            """)

            # Events Table
            cursor.execute("""
            CREATE TABLE IF NOT EXISTS events (
                id INTEGER PRIMARY KEY AUTOINCREMENT,
                timestamp TEXT NOT NULL,
                device_id TEXT NOT NULL,
                event_type TEXT NOT NULL,
                severity TEXT NOT NULL,
                description TEXT,
                metadata TEXT,
                created_at TEXT NOT NULL
            );
            """)

            # Calibration Table
            cursor.execute("""
            CREATE TABLE IF NOT EXISTS calibration (
                key TEXT PRIMARY KEY,
                value REAL NOT NULL,
                unit TEXT,
                notes TEXT,
                updated_at TEXT NOT NULL
            );
            """)

            # FCM Notification Tokens
            cursor.execute("""
            CREATE TABLE IF NOT EXISTS notification_tokens (
                token TEXT PRIMARY KEY,
                device_id TEXT,
                created_at TEXT NOT NULL,
                last_seen TEXT NOT NULL
            );
            """)

            # Indexes
            cursor.execute("CREATE INDEX IF NOT EXISTS idx_telemetry_time ON telemetry (timestamp);")
            cursor.execute("CREATE INDEX IF NOT EXISTS idx_telemetry_device ON telemetry (device_id);")
            cursor.execute("CREATE INDEX IF NOT EXISTS idx_events_time ON events (timestamp);")
            conn.commit()

    def save_telemetry(self, msg: TelemetryMessage, backend_status: BackendStatus) -> int:
        with self.get_connection() as conn:
            cursor = conn.cursor()
            created_at = datetime.now(timezone.utc).isoformat()
            raw_payload = msg.model_dump_json()

            cursor.execute("""
            INSERT INTO telemetry (
                device_id, timestamp, schema_version,
                ultrasonic_raw, ultrasonic_median, ultrasonic_filtered,
                turbidity_raw, turbidity_ntu, oil_estimated_mg_l,
                loadcell_raw, oil_mass_g,
                surface_height_cm, accumulated_volume_ml, critical_volume_ml,
                motor_pwm, motor_state, heater_state, solenoid_state,
                embedded_state, backend_status, sensor_fault, safety_state,
                wifi_connected, mqtt_connected, raw_payload, created_at
            ) VALUES (?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?)
            """, (
                msg.device_id,
                msg.timestamp.isoformat(),
                msg.schema_version,
                msg.sensors.ultrasonic_raw,
                msg.sensors.ultrasonic_median,
                msg.sensors.ultrasonic_filtered,
                msg.sensors.turbidity_raw,
                msg.sensors.turbidity_ntu,
                msg.sensors.oil_estimated_mg_l,
                msg.sensors.loadcell_raw,
                msg.sensors.oil_mass_g,
                msg.model.surface_height_cm,
                msg.model.accumulated_volume_ml,
                msg.model.critical_volume_ml,
                msg.actuators.motor_pwm,
                msg.actuators.motor_state.value,
                msg.actuators.heater_state.value,
                msg.actuators.solenoid_state.value,
                msg.system.embedded_state.value,
                backend_status.value,
                1 if msg.system.sensor_fault else 0,
                msg.system.safety_state,
                1 if msg.communication.wifi_connected else 0,
                1 if msg.communication.mqtt_connected else 0,
                raw_payload,
                created_at
            ))
            conn.commit()
            return cursor.lastrowid

    def get_latest_telemetry(self, device_id: Optional[str] = None) -> Optional[Dict[str, Any]]:
        with self.get_connection() as conn:
            cursor = conn.cursor()
            if device_id:
                cursor.execute(
                    "SELECT raw_payload, backend_status FROM telemetry WHERE device_id = ? ORDER BY id DESC LIMIT 1",
                    (device_id,)
                )
            else:
                cursor.execute("SELECT raw_payload, backend_status FROM telemetry ORDER BY id DESC LIMIT 1")
            row = cursor.fetchone()
            if not row:
                return None
            data = json.loads(row["raw_payload"])
            data["system"]["backend_status"] = row["backend_status"]
            return data

    def get_telemetry_history(
        self,
        device_id: Optional[str] = None,
        limit: int = 100,
        offset: int = 0,
        since: Optional[datetime] = None,
        until: Optional[datetime] = None,
    ) -> List[Dict[str, Any]]:
        """Return stored telemetry in chronological order.

        ``since``/``until`` bound the device ``timestamp`` (inclusive) and are
        compared via SQLite ``julianday()`` so ISO strings with or without
        microseconds/offsets sort correctly. Naive datetimes are treated as UTC.
        """
        clauses: List[str] = []
        params: List[Any] = []
        if device_id:
            clauses.append("device_id = ?")
            params.append(device_id)
        if since is not None:
            clauses.append("julianday(timestamp) >= julianday(?)")
            params.append(_as_utc_iso(since))
        if until is not None:
            clauses.append("julianday(timestamp) <= julianday(?)")
            params.append(_as_utc_iso(until))
        where = f" WHERE {' AND '.join(clauses)}" if clauses else ""

        with self.get_connection() as conn:
            cursor = conn.cursor()
            cursor.execute(
                f"SELECT raw_payload, backend_status FROM telemetry{where} ORDER BY id DESC LIMIT ? OFFSET ?",
                (*params, limit, offset)
            )
            rows = cursor.fetchall()
            results = []
            for row in rows:
                data = json.loads(row["raw_payload"])
                data["system"]["backend_status"] = row["backend_status"]
                results.append(data)
            # Return in chronological order
            results.reverse()
            return results

    def log_event(
        self,
        event_type: str,
        severity: str,
        description: str,
        device_id: str = "SGT-001",
        metadata: Optional[Dict[str, Any]] = None
    ) -> int:
        with self.get_connection() as conn:
            cursor = conn.cursor()
            now = datetime.now(timezone.utc).isoformat()
            cursor.execute("""
            INSERT INTO events (timestamp, device_id, event_type, severity, description, metadata, created_at)
            VALUES (?, ?, ?, ?, ?, ?, ?)
            """, (
                now,
                device_id,
                event_type,
                severity,
                description,
                json.dumps(metadata or {}),
                now
            ))
            conn.commit()
            return cursor.lastrowid

    def get_events(self, limit: int = 50, offset: int = 0) -> List[Dict[str, Any]]:
        with self.get_connection() as conn:
            cursor = conn.cursor()
            cursor.execute(
                "SELECT id, timestamp, device_id, event_type, severity, description, metadata FROM events ORDER BY id DESC LIMIT ? OFFSET ?",
                (limit, offset)
            )
            rows = cursor.fetchall()
            return [dict(row) for row in rows]

    def set_calibration(
        self,
        key: str,
        value: float,
        unit: str = "",
        notes: str = "",
    ) -> None:
        """Upsert a calibration value (versioned via updated_at)."""
        with self.get_connection() as conn:
            cursor = conn.cursor()
            now = datetime.now(timezone.utc).isoformat()
            cursor.execute("""
            INSERT INTO calibration (key, value, unit, notes, updated_at)
            VALUES (?, ?, ?, ?, ?)
            ON CONFLICT(key) DO UPDATE SET
                value=excluded.value,
                unit=excluded.unit,
                notes=excluded.notes,
                updated_at=excluded.updated_at
            """, (key, value, unit, notes, now))
            conn.commit()

    def get_calibration(self, key: Optional[str] = None) -> List[Dict[str, Any]]:
        with self.get_connection() as conn:
            cursor = conn.cursor()
            if key:
                cursor.execute(
                    "SELECT key, value, unit, notes, updated_at FROM calibration WHERE key = ?",
                    (key,)
                )
            else:
                cursor.execute("SELECT key, value, unit, notes, updated_at FROM calibration ORDER BY key")
            rows = cursor.fetchall()
            return [dict(row) for row in rows]

    def register_notification_token(self, token: str, device_id: str = "SGT-001") -> None:
        with self.get_connection() as conn:
            cursor = conn.cursor()
            now = datetime.now(timezone.utc).isoformat()
            cursor.execute("""
            INSERT INTO notification_tokens (token, device_id, created_at, last_seen)
            VALUES (?, ?, ?, ?)
            ON CONFLICT(token) DO UPDATE SET
                device_id=excluded.device_id,
                last_seen=excluded.last_seen
            """, (token, device_id, now, now))
            conn.commit()

    def get_notification_tokens(self) -> List[Dict[str, Any]]:
        with self.get_connection() as conn:
            cursor = conn.cursor()
            cursor.execute("SELECT token, device_id, created_at, last_seen FROM notification_tokens ORDER BY created_at")
            return [dict(row) for row in cursor.fetchall()]

    def remove_notification_token(self, token: str) -> bool:
        with self.get_connection() as conn:
            cursor = conn.cursor()
            cursor.execute("DELETE FROM notification_tokens WHERE token = ?", (token,))
            conn.commit()
            return cursor.rowcount > 0
