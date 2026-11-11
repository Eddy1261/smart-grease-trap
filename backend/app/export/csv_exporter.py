"""Structured CSV telemetry export — schema v1 (LOCKED).

This is the Halim -> Risza handoff dataset contract (audit §15).
Do NOT rename or reorder columns silently. Any change requires an explicit
schema version bump and documentation in docs/csv_schema_v1.md.

Schema v1 — 25 columns, deterministic order:
    schema_version, timestamp, device_id, scenario_id,
    raw_ultrasonic, median_ultrasonic, ema_ultrasonic,
    height, rate, filtered_rate, accumulated_volume,
    turbidity_raw, turbidity_voltage, estimated_oil_fat,
    load_cell_raw, collector_mass,
    motor_pwm, motor_state, heater_state, solenoid_state,
    embedded_fsm_state, backend_status, fault_code,
    wifi_status, mqtt_status

Conventions (locked):
    - missing/unavailable value  -> empty field ("")
    - timestamp                  -> ISO 8601 UTC, as stored in telemetry
    - rate / filtered_rate       -> empty in v1: AVM-internal values are not
                                    part of telemetry schema v1 (see docs)
    - turbidity_raw / turbidity_voltage -> both carry the telemetry voltage;
                                    raw ADC counts are firmware-internal in v1
    - fault_code                 -> "CHANNEL_STATE" tokens joined by "|",
                                    e.g. "ULTRASONIC_OUT_OF_RANGE|TURBIDITY_DISCONNECTED";
                                    "SENSOR_FAULT" when the fault flag is set
                                    without per-channel detail; empty otherwise
    - wifi_status / mqtt_status  -> "CONNECTED" / "DISCONNECTED"
"""
import argparse
import csv
from datetime import datetime, timezone
from pathlib import Path
from typing import Any, Dict, Iterable, List, Optional

CSV_SCHEMA_VERSION = 1

CSV_COLUMNS = [
    "schema_version",
    "timestamp",
    "device_id",
    "scenario_id",
    "raw_ultrasonic",
    "median_ultrasonic",
    "ema_ultrasonic",
    "height",
    "rate",
    "filtered_rate",
    "accumulated_volume",
    "turbidity_raw",
    "turbidity_voltage",
    "estimated_oil_fat",
    "load_cell_raw",
    "collector_mass",
    "motor_pwm",
    "motor_state",
    "heater_state",
    "solenoid_state",
    "embedded_fsm_state",
    "backend_status",
    "fault_code",
    "wifi_status",
    "mqtt_status",
]

CSV_UNITS = {
    "schema_version": "int",
    "timestamp": "ISO8601 UTC",
    "device_id": "string",
    "scenario_id": "string",
    "raw_ultrasonic": "cm",
    "median_ultrasonic": "cm",
    "ema_ultrasonic": "cm",
    "height": "cm",
    "rate": "cm/s (unavailable in telemetry v1)",
    "filtered_rate": "cm/s (unavailable in telemetry v1)",
    "accumulated_volume": "mL",
    "turbidity_raw": "V (telemetry v1 stores voltage; ADC counts firmware-internal)",
    "turbidity_voltage": "V",
    "estimated_oil_fat": "mg/L (proxy estimate)",
    "load_cell_raw": "ADC counts",
    "collector_mass": "g",
    "motor_pwm": "duty 0-255",
    "motor_state": "OFF/ON",
    "heater_state": "OFF/ON",
    "solenoid_state": "OPEN/CLOSED",
    "embedded_fsm_state": "enum",
    "backend_status": "enum",
    "fault_code": "string",
    "wifi_status": "CONNECTED/DISCONNECTED",
    "mqtt_status": "CONNECTED/DISCONNECTED",
}

MISSING = ""


def _get(record: Dict[str, Any], dotted_path: str, default: Any = MISSING) -> Any:
    node: Any = record
    for part in dotted_path.split("."):
        if not isinstance(node, dict) or part not in node:
            return default
        node = node[part]
    if node is None:
        return default
    return node


def derive_fault_code(record: Dict[str, Any]) -> str:
    codes: List[str] = []
    health = _get(record, "sensors.sensor_health", default={})
    if isinstance(health, dict):
        for channel in ("ultrasonic", "turbidity", "loadcell"):
            state = health.get(channel, "VALID")
            if state and state != "VALID":
                codes.append(f"{channel.upper()}_{state}")
    if not codes and _get(record, "system.sensor_fault", default=False):
        codes.append("SENSOR_FAULT")
    return "|".join(codes)


def _status_token(connected: Any) -> str:
    if connected is None:
        return MISSING
    return "CONNECTED" if connected else "DISCONNECTED"


def telemetry_to_row(record: Dict[str, Any], scenario_id: str = "") -> List[Any]:
    """Map one stored telemetry dict to a schema-v1 CSV row."""
    turbidity_v = _get(record, "sensors.turbidity_raw")
    return [
        CSV_SCHEMA_VERSION,
        _get(record, "timestamp"),
        _get(record, "device_id"),
        scenario_id,
        _get(record, "sensors.ultrasonic_raw"),
        _get(record, "sensors.ultrasonic_median"),
        _get(record, "sensors.ultrasonic_filtered"),
        _get(record, "model.surface_height_cm"),
        MISSING,  # rate: AVM-internal, not in telemetry v1
        MISSING,  # filtered_rate: AVM-internal, not in telemetry v1
        _get(record, "model.accumulated_volume_ml"),
        turbidity_v,
        turbidity_v,  # telemetry v1 turbidity_raw already stores voltage
        _get(record, "sensors.oil_estimated_mg_l"),
        _get(record, "sensors.loadcell_raw"),
        _get(record, "sensors.oil_mass_g"),
        _get(record, "actuators.motor_pwm"),
        _get(record, "actuators.motor_state"),
        _get(record, "actuators.heater_state"),
        _get(record, "actuators.solenoid_state"),
        _get(record, "system.embedded_state"),
        _get(record, "system.backend_status"),
        derive_fault_code(record),
        _status_token(_get(record, "communication.wifi_connected", default=None)),
        _status_token(_get(record, "communication.mqtt_connected", default=None)),
    ]


def _header_lines() -> List[str]:
    lines = [
        f"# Smart Grease Trap telemetry export — CSV schema_version={CSV_SCHEMA_VERSION}",
        "# Columns: " + ",".join(CSV_COLUMNS),
        "# Units: " + "; ".join(f"{col}={CSV_UNITS[col]}" for col in CSV_COLUMNS),
        "# Missing values are represented as empty fields.",
    ]
    return lines


def export_records(
    records: Iterable[Dict[str, Any]],
    output_path: str,
    scenario_id: str = "",
) -> str:
    """Write deterministic schema-v1 CSV. Returns the output path."""
    path = Path(output_path)
    path.parent.mkdir(parents=True, exist_ok=True)

    with path.open("w", newline="", encoding="utf-8") as fh:
        for line in _header_lines():
            fh.write(line + "\n")
        writer = csv.writer(fh)
        writer.writerow(CSV_COLUMNS)
        for record in records:
            writer.writerow(telemetry_to_row(record, scenario_id=scenario_id))
    return str(path)


def export_from_db(
    db,
    output_path: str,
    device_id: Optional[str] = None,
    limit: int = 1000,
    offset: int = 0,
    scenario_id: str = "",
    since: Optional[datetime] = None,
    until: Optional[datetime] = None,
) -> str:
    """Export stored telemetry from a DatabaseManager in chronological order.

    ``since``/``until`` are inclusive bounds on the device timestamp (naive
    values are treated as UTC). They make per-run Phase E exports unambiguous.
    """
    if since is not None and until is not None:
        if since.tzinfo is None:
            since = since.replace(tzinfo=timezone.utc)
        if until.tzinfo is None:
            until = until.replace(tzinfo=timezone.utc)
        if since > until:
            raise ValueError("since must be earlier than or equal to until")
    records = db.get_telemetry_history(
        device_id=device_id, limit=limit, offset=offset, since=since, until=until
    )
    return export_records(records, output_path, scenario_id=scenario_id)


def _parse_timestamp_arg(value: str) -> datetime:
    """Parse an ISO-8601 CLI timestamp; accepts a trailing 'Z' (Python 3.10)."""
    text = value.strip()
    if text.endswith(("Z", "z")):
        text = text[:-1] + "+00:00"
    try:
        parsed = datetime.fromisoformat(text)
    except ValueError as exc:
        raise argparse.ArgumentTypeError(
            f"invalid ISO-8601 timestamp: {value!r} (expected e.g. 2026-10-29T08:30:00Z)"
        ) from exc
    if parsed.tzinfo is None:
        parsed = parsed.replace(tzinfo=timezone.utc)
    return parsed


def main():
    parser = argparse.ArgumentParser(description="Smart Grease Trap CSV telemetry exporter (schema v1)")
    parser.add_argument("--output", required=True, help="Output CSV path")
    parser.add_argument("--device-id", default=None, help="Filter by device ID")
    parser.add_argument(
        "--limit", type=int, default=None,
        help="Max records to export (default 1000; 100000 when --since/--until is used)",
    )
    parser.add_argument("--offset", type=int, default=0, help="Offset into history")
    parser.add_argument("--scenario-id", default="", help="Scenario ID stamped into every row")
    parser.add_argument(
        "--since", type=_parse_timestamp_arg, default=None,
        help="Inclusive lower bound on telemetry timestamp (ISO-8601, e.g. 2026-10-29T08:30:00Z)",
    )
    parser.add_argument(
        "--until", type=_parse_timestamp_arg, default=None,
        help="Inclusive upper bound on telemetry timestamp (ISO-8601)",
    )
    args = parser.parse_args()

    limit = args.limit
    if limit is None:
        limit = 100000 if (args.since or args.until) else 1000

    from backend.app.db.database import DatabaseManager

    path = export_from_db(
        DatabaseManager(),
        args.output,
        device_id=args.device_id,
        limit=limit,
        offset=args.offset,
        scenario_id=args.scenario_id,
        since=args.since,
        until=args.until,
    )
    print(f"[+] CSV exported: {path}")


if __name__ == "__main__":
    main()
