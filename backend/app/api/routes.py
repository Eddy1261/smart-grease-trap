import os
from pathlib import Path
from typing import Optional, List, Dict, Any, Literal
from fastapi import APIRouter, HTTPException, Query, Request, Response, status
from fastapi.responses import FileResponse
from pydantic import BaseModel, Field

from backend.app.db.database import DatabaseManager
from backend.app.reports.pdf_generator import ComplianceReportGenerator
from backend.app.config import settings
from backend.app.notifications.fcm_service import FCMNotificationService, build_web_config

router = APIRouter()
db = DatabaseManager()
report_gen = ComplianceReportGenerator()


def _notification_service() -> FCMNotificationService:
    return FCMNotificationService(db=db)

class ReportGenerationRequest(BaseModel):
    scenario_name: str = "Standard Simulation"
    device_id: str = "SGT-001"
    limit_records: int = 100


class NotificationRegisterRequest(BaseModel):
    token: str = Field(..., min_length=10, description="FCM registration token from the dashboard")
    device_id: str = "SGT-001"


class ManualDischargeRequest(BaseModel):
    state: Literal["open", "close"]

@router.get("/health", tags=["Health"])
def health_check():
    return {
        "status": "healthy",
        "service": "smart-grease-trap-backend",
        "version": "0.1.0"
    }

@router.get("/api/status", tags=["Status"])
def get_system_status():
    latest = db.get_latest_telemetry()
    if not latest:
        return {
            "status": "AWAITING_TELEMETRY",
            "device_id": None,
            "timestamp": None,
            "actuators": None,
            "sensors": None,
            "thresholds": {
                "container_near_full_g": settings.THRESHOLD_CONTAINER_NEAR_FULL_G,
                "container_max_mass_g": settings.THRESHOLD_CONTAINER_MAX_MASS_G
            }
        }
    return {
        "status": latest.get("system", {}).get("backend_status", "UNKNOWN"),
        "embedded_state": latest.get("system", {}).get("embedded_state"),
        "device_id": latest.get("device_id"),
        "timestamp": latest.get("timestamp"),
        "actuators": latest.get("actuators"),
        "sensors": latest.get("sensors"),
        "model": latest.get("model"),
        "communication": latest.get("communication"),
        "thresholds": {
            "container_near_full_g": settings.THRESHOLD_CONTAINER_NEAR_FULL_G,
            "container_max_mass_g": settings.THRESHOLD_CONTAINER_MAX_MASS_G
        }
    }

@router.get("/api/telemetry/latest", tags=["Telemetry"])
def get_latest_telemetry(device_id: Optional[str] = None):
    data = db.get_latest_telemetry(device_id=device_id)
    if not data:
        raise HTTPException(status_code=404, detail="No telemetry available")
    return data

@router.get("/api/telemetry/history", tags=["Telemetry"])
def get_telemetry_history(
    device_id: Optional[str] = None,
    limit: int = Query(100, ge=1, le=500),
    offset: int = Query(0, ge=0)
):
    history = db.get_telemetry_history(device_id=device_id, limit=limit, offset=offset)
    return {
        "count": len(history),
        "limit": limit,
        "offset": offset,
        "data": history
    }

@router.get("/api/events", tags=["Events"])
def get_events(
    limit: int = Query(50, ge=1, le=200),
    offset: int = Query(0, ge=0)
):
    events = db.get_events(limit=limit, offset=offset)
    return {
        "count": len(events),
        "data": events
    }

@router.post("/api/actuators/manual-discharge", tags=["Actuators"])
def manual_discharge(req: ManualDischargeRequest, request: Request):
    """Operator override: open/close the discharge valve + booster pump.

    The firmware bypasses C_oil/volume/FSM gates while the command is latched,
    but still enforces its own fail-safe (sensor fault, MQTT loss, reboot).
    """
    subscriber = getattr(request.app.state, "mqtt_subscriber", None)
    command = {"action": "manual_discharge", "state": req.state}
    if subscriber is None or not subscriber.publish_command(command):
        raise HTTPException(
            status_code=503,
            detail="MQTT broker unavailable; manual discharge command was not sent",
        )
    db.log_event(
        "MANUAL_DISCHARGE",
        "WARNING",
        f"Manual discharge {req.state} requested from dashboard",
        metadata=command,
    )
    return {"sent": True, "state": req.state}


@router.post("/api/reports/generate", tags=["Reports"])
def generate_report(req: ReportGenerationRequest):
    history = db.get_telemetry_history(device_id=req.device_id, limit=req.limit_records)
    events = db.get_events(limit=50)

    try:
        filepath = report_gen.generate(
            telemetry_records=history,
            events=events,
            scenario_name=req.scenario_name,
            device_id=req.device_id
        )
        filename = Path(filepath).name
        return {
            "message": "Report generated successfully",
            "filename": filename,
            "download_url": f"/api/reports/download/{filename}"
        }
    except Exception as e:
        raise HTTPException(status_code=500, detail=f"Failed to generate report: {str(e)}")

@router.get("/api/reports/download/{filename}", tags=["Reports"])
def download_report(filename: str):
    reports_dir = Path(settings.REPORTS_DIR).resolve()

    # Only a plain PDF filename inside REPORTS_DIR is allowed. This rejects
    # path separators, absolute paths and Windows backslash traversal (e.g.
    # "..%5C..%5C.env") before any filesystem access.
    try:
        candidate = Path(filename)
    except (TypeError, ValueError):
        raise HTTPException(status_code=404, detail="Report file not found")

    if (
        candidate.name != filename
        or filename in {".", ".."}
        or candidate.suffix.lower() != ".pdf"
    ):
        raise HTTPException(status_code=404, detail="Report file not found")

    try:
        file_path = (reports_dir / filename).resolve()
    except (OSError, ValueError):
        raise HTTPException(status_code=404, detail="Report file not found")

    if not file_path.is_relative_to(reports_dir) or not file_path.is_file():
        raise HTTPException(status_code=404, detail="Report file not found")

    return FileResponse(
        path=str(file_path),
        media_type="application/pdf",
        filename=filename
    )


# ============================================================================
# FCM NOTIFICATIONS
# ============================================================================

@router.post("/api/notifications/register", tags=["Notifications"])
def register_notification_token(req: NotificationRegisterRequest):
    db.register_notification_token(req.token, req.device_id)
    return {"registered": True, "device_id": req.device_id}


@router.delete("/api/notifications/register", tags=["Notifications"])
def unregister_notification_token(token: str = Query(..., min_length=10)):
    removed = db.remove_notification_token(token)
    if not removed:
        raise HTTPException(status_code=404, detail="Token not registered")
    return {"unregistered": True}


@router.get("/api/notifications/config", tags=["Notifications"])
def notification_config():
    """Public client config for the dashboard. Never exposes private credentials."""
    return {
        "enabled": _notification_service().client_config_available(),
        "delivery_ready": _notification_service().server_delivery_available(),
        "vapid_key": settings.FCM_VAPID_KEY or None,
        "firebase": build_web_config(),
    }


@router.post("/api/notifications/test", tags=["Notifications"])
def send_test_notification():
    """Manual delivery test. In stub mode returns stub=True (no real delivery)."""
    tokens = [row["token"] for row in db.get_notification_tokens()]
    result = _notification_service().send_to_tokens(
        tokens,
        "Smart Grease Trap test notification",
        "Manual delivery test from the backend service.",
        {"kind": "test"},
    )
    return result.as_dict()
