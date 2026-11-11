"""Firmware <-> backend telemetry contract test.

Compiles the REAL firmware JSON builder (no mocks), runs it, and validates
both emitted snapshots against the REAL Pydantic schema. This is the proof
that what the ESP32 publishes is exactly what the backend accepts.
"""
import json
import shutil
import subprocess
import tempfile
from pathlib import Path

import pytest

from backend.app.schemas.telemetry import TelemetryMessage

REPO_ROOT = Path(__file__).resolve().parent.parent.parent


def test_firmware_telemetry_json_validates_against_backend_schema():
    if shutil.which("g++") is None:
        pytest.skip("g++ not available for contract emission")

    src = REPO_ROOT / "firmware" / "test" / "contract_emit.cpp"
    include = REPO_ROOT / "firmware" / "include"
    with tempfile.TemporaryDirectory() as tmp:
        exe = str(Path(tmp) / ("contract_emit.exe" if shutil.which("where") else "contract_emit"))
        compiled = subprocess.run(
            ["g++", "-std=c++17", "-I", str(include), str(src), "-o", exe],
            capture_output=True, text=True,
        )
        assert compiled.returncode == 0, compiled.stderr

        emitted = subprocess.run([exe], capture_output=True, text=True)
        assert emitted.returncode == 0, emitted.stderr

    lines = [ln for ln in emitted.stdout.splitlines() if ln.strip()]
    assert len(lines) == 3, f"expected 3 snapshots, got {len(lines)}"

    calibrated = TelemetryMessage.model_validate(json.loads(lines[0]))
    assert calibrated.sensors.turbidity_ntu == pytest.approx(42.5)
    assert calibrated.sensors.oil_estimated_mg_l is None
    assert calibrated.system.embedded_state.value == "MONITORING"

    uncalibrated = TelemetryMessage.model_validate(json.loads(lines[1]))
    assert uncalibrated.sensors.turbidity_ntu is None
    assert uncalibrated.system.embedded_state.value == "PROCESS_TIMEOUT"
    assert uncalibrated.system.maintenance_required is True

    oil = TelemetryMessage.model_validate(json.loads(lines[2]))
    assert oil.sensors.turbidity_ntu is None
    assert oil.sensors.oil_estimated_mg_l == pytest.approx(241.3)
