"""Scenario runner / replay tests (Step 7 — Halim Master Audit, §16)."""
import csv
import json

from simulator.run_scenarios import (
    S1_S8_ORDER,
    derive_events,
    replay_telemetry,
    run_all,
    run_scenario,
)


def _read_csv_rows(path):
    with open(path, "r", newline="", encoding="utf-8") as fh:
        lines = [ln for ln in fh if not ln.startswith("#")]
    return list(csv.reader(lines))


def test_run_scenario_produces_all_artifacts(tmp_path):
    result = run_scenario("s1_pure_oil", cycles=15, seed=42, output_dir=str(tmp_path))
    directory = tmp_path / "s1_pure_oil"

    for name in ("metadata.json", "telemetry.jsonl", "events.json",
                 "actuator_profile.json", "telemetry.csv"):
        assert (directory / name).exists(), f"missing {name}"

    metadata = json.loads((directory / "metadata.json").read_text(encoding="utf-8"))
    assert metadata["synthetic"] is True
    assert "NOT physical experiment validation" in metadata["disclaimer"]
    assert metadata["scenario_id"] == "s1_pure_oil"
    assert metadata["seed"] == 42
    assert metadata["cycles"] == 15

    lines = (directory / "telemetry.jsonl").read_text(encoding="utf-8").strip().splitlines()
    assert len(lines) == 15

    rows = _read_csv_rows(directory / "telemetry.csv")
    assert len(rows) == 16  # header + 15
    assert rows[0][3] == "scenario_id"
    assert all(row[3] == "s1_pure_oil" for row in rows[1:])


def test_runner_is_deterministic(tmp_path):
    run_scenario("s4_turbulence", cycles=20, seed=123, output_dir=str(tmp_path / "a"))
    run_scenario("s4_turbulence", cycles=20, seed=123, output_dir=str(tmp_path / "b"))

    a = (tmp_path / "a" / "s4_turbulence" / "telemetry.jsonl").read_bytes()
    b = (tmp_path / "b" / "s4_turbulence" / "telemetry.jsonl").read_bytes()
    assert a == b

    a_csv = (tmp_path / "a" / "s4_turbulence" / "telemetry.csv").read_bytes()
    b_csv = (tmp_path / "b" / "s4_turbulence" / "telemetry.csv").read_bytes()
    assert a_csv == b_csv

    a_events = (tmp_path / "a" / "s4_turbulence" / "events.json").read_bytes()
    b_events = (tmp_path / "b" / "s4_turbulence" / "events.json").read_bytes()
    assert a_events == b_events


def test_run_all_s1_to_s8(tmp_path):
    results = run_all(cycles=10, seed=100, output_dir=str(tmp_path))
    assert [r["scenario_id"] for r in results] == S1_S8_ORDER
    for key in S1_S8_ORDER:
        assert (tmp_path / key / "telemetry.csv").exists()
        assert (tmp_path / key / "metadata.json").exists()
    assert (tmp_path / "index.json").exists()


def test_s1_events_include_skimming_and_transitions(tmp_path):
    result = run_scenario("s1_pure_oil", cycles=40, seed=7, output_dir=str(tmp_path))
    events = json.loads((tmp_path / "s1_pure_oil" / "events.json").read_text(encoding="utf-8"))
    types = {e["event_type"] for e in events}
    assert "SKIMMING_STARTED" in types
    assert "STATUS_TRANSITION" in types


def test_sensor_fault_scenario_emits_fault_event(tmp_path):
    run_scenario("sensor_fault", cycles=5, seed=1, output_dir=str(tmp_path))
    events = json.loads((tmp_path / "sensor_fault" / "events.json").read_text(encoding="utf-8"))
    assert any(e["event_type"] == "SENSOR_FAULT" for e in events)


def test_heavy_emulsion_emits_maintenance_event(tmp_path):
    run_scenario("s3_heavy_emulsion", cycles=60, seed=1, output_dir=str(tmp_path))
    events = json.loads((tmp_path / "s3_heavy_emulsion" / "events.json").read_text(encoding="utf-8"))
    maintenance = [e for e in events if e["event_type"] == "MAINTENANCE_REQUIRED"]
    assert len(maintenance) == 1  # one-shot, not per-cycle


def test_replay_matches_original_csv(tmp_path):
    run_scenario("s2_light_emulsion", cycles=25, seed=55, output_dir=str(tmp_path / "original"))
    replay_telemetry(
        str(tmp_path / "original" / "s2_light_emulsion" / "telemetry.jsonl"),
        output_dir=str(tmp_path / "replayed"),
        scenario_id="s2_light_emulsion",
    )

    original = (tmp_path / "original" / "s2_light_emulsion" / "telemetry.csv").read_bytes()
    replayed = (tmp_path / "replayed" / "s2_light_emulsion" / "telemetry.csv").read_bytes()
    assert original == replayed

    original_events = json.loads(
        (tmp_path / "original" / "s2_light_emulsion" / "events.json").read_text(encoding="utf-8"))
    replayed_events = json.loads(
        (tmp_path / "replayed" / "s2_light_emulsion" / "events.json").read_text(encoding="utf-8"))
    assert original_events == replayed_events


def test_derive_events_handles_empty():
    assert derive_events([]) == []
