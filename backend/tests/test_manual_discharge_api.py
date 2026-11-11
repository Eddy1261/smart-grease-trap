from fastapi.testclient import TestClient

from backend.app.main import app
from backend.app.api import routes


client = TestClient(app)


class FakeSubscriber:
    def __init__(self, ok: bool):
        self.ok = ok
        self.commands = []

    def publish_command(self, command):
        self.commands.append(command)
        return self.ok


def _post(state):
    return client.post("/api/actuators/manual-discharge", json={"state": state})


def test_manual_discharge_returns_503_when_broker_is_down():
    original = app.state.mqtt_subscriber
    app.state.mqtt_subscriber = FakeSubscriber(ok=False)
    try:
        response = _post("open")
        assert response.status_code == 503
        assert "MQTT broker unavailable" in response.json()["detail"]
    finally:
        app.state.mqtt_subscriber = original


def test_manual_discharge_publishes_open_and_close(monkeypatch):
    fake = FakeSubscriber(ok=True)
    original = app.state.mqtt_subscriber
    app.state.mqtt_subscriber = fake
    logged = []
    monkeypatch.setattr(
        routes.db,
        "log_event",
        lambda *args, **kwargs: logged.append((args, kwargs)) or 1,
    )
    try:
        for state in ("open", "close"):
            response = _post(state)
            assert response.status_code == 200
            assert response.json() == {"sent": True, "state": state}

        assert fake.commands == [
            {"action": "manual_discharge", "state": "open"},
            {"action": "manual_discharge", "state": "close"},
        ]
        assert len(logged) == 2
        assert logged[0][0][0] == "MANUAL_DISCHARGE"
    finally:
        app.state.mqtt_subscriber = original


def test_manual_discharge_rejects_invalid_state():
    response = _post("maybe")
    assert response.status_code == 422
