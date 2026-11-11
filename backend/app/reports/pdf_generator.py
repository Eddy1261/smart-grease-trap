import os
from pathlib import Path
from datetime import datetime, timezone
from typing import List, Dict, Any, Optional

from reportlab.lib.pagesizes import letter
from reportlab.lib import colors
from reportlab.lib.styles import getSampleStyleSheet, ParagraphStyle
from reportlab.platypus import SimpleDocTemplate, Paragraph, Spacer, Table, TableStyle, KeepTogether

from backend.app.config import settings

class ComplianceReportGenerator:
    """
    Generates compliance-oriented PDF evaluation reports using ReportLab.
    Strictly follows project rules: distinguishes turbidity proxy measurements
    from analytical gravimetric reference standards.
    """

    def __init__(self, output_dir: str = settings.REPORTS_DIR):
        self.output_dir = Path(output_dir)
        self.output_dir.mkdir(parents=True, exist_ok=True)

    def generate(
        self,
        telemetry_records: List[Dict[str, Any]],
        events: List[Dict[str, Any]],
        scenario_name: str = "Standard Operation",
        device_id: str = "SGT-001"
    ) -> str:
        timestamp_str = datetime.now(timezone.utc).strftime("%Y%m%d_%H%M%S")
        filename = f"report_{device_id}_{timestamp_str}.pdf"
        filepath = self.output_dir / filename

        doc = SimpleDocTemplate(
            str(filepath),
            pagesize=letter,
            rightMargin=36,
            leftMargin=36,
            topMargin=36,
            bottomMargin=36
        )

        styles = getSampleStyleSheet()
        title_style = ParagraphStyle(
            'ReportTitle',
            parent=styles['Heading1'],
            fontSize=18,
            leading=22,
            textColor=colors.HexColor("#1e293b"),
            spaceAfter=6
        )
        subtitle_style = ParagraphStyle(
            'ReportSubtitle',
            parent=styles['Normal'],
            fontSize=11,
            leading=14,
            textColor=colors.HexColor("#64748b"),
            spaceAfter=14
        )
        heading2_style = ParagraphStyle(
            'ReportH2',
            parent=styles['Heading2'],
            fontSize=13,
            leading=16,
            textColor=colors.HexColor("#0f172a"),
            spaceBefore=12,
            spaceAfter=6
        )
        body_style = ParagraphStyle(
            'ReportBody',
            parent=styles['Normal'],
            fontSize=9,
            leading=13,
            textColor=colors.HexColor("#334155")
        )
        disclaimer_style = ParagraphStyle(
            'Disclaimer',
            parent=styles['Normal'],
            fontSize=8,
            leading=11,
            textColor=colors.HexColor("#b91c1c"),
            backColor=colors.HexColor("#fef2f2"),
            borderColor=colors.HexColor("#f87171"),
            borderWidth=1,
            borderPadding=6,
            spaceBefore=10,
            spaceAfter=10
        )

        elements = []

        # Header / Title
        elements.append(Paragraph("SMART GREASE TRAP (CAPSTONE C-251)", title_style))
        elements.append(Paragraph(
            f"Automated Wastewater Separation Compliance & Telemetry Report<br/>"
            f"Device: <b>{device_id}</b> | Scenario: <b>{scenario_name}</b> | Generated: <b>{datetime.now(timezone.utc).strftime('%Y-%m-%d %H:%M:%S UTC')}</b>",
            subtitle_style
        ))

        # Important Regulatory Notice & Disclaimer
        elements.append(Paragraph(
            "<b>REGULATORY AND METHODOLOGICAL NOTICE:</b><br/>"
            "This report analyzes real-time sensor proxy estimates. Optical turbidity measurements (SEN0189) "
            "serve as a continuous operational proxy for oil/fat concentration in kitchen wastewater. "
            "Under C-251 engineering principles, sensor-derived proxy metrics do NOT independently establish "
            "statutory regulatory compliance. Formal compliance requires gravimetric analytical verification (SNI 6989.10:2011).",
            disclaimer_style
        ))

        # Executive Summary
        elements.append(Paragraph("1. Executive Summary & Telemetry Statistics", heading2_style))
        total_records = len(telemetry_records)
        skimming_events = [r for r in telemetry_records if r.get("actuators", {}).get("motor_state") == "ON" or r.get("actuators", {}).get("motor_pwm", 0) > 0]
        fault_records = [r for r in telemetry_records if r.get("system", {}).get("sensor_fault")]

        avg_vol = 0.0
        max_mass = 0.0
        avg_oil = None
        oil_exceed_cycles = 0
        avg_ntu = None
        oil_values = []
        ntu_values = []
        if total_records > 0:
            avg_vol = sum(r.get("model", {}).get("accumulated_volume_ml", 0) for r in telemetry_records) / total_records
            max_mass = max(r.get("sensors", {}).get("oil_mass_g", 0) for r in telemetry_records)
            oil_values = [
                r.get("sensors", {}).get("oil_estimated_mg_l")
                for r in telemetry_records
                if isinstance(r.get("sensors", {}).get("oil_estimated_mg_l"), (int, float))
            ]
            if oil_values:
                avg_oil = sum(oil_values) / len(oil_values)
                oil_exceed_cycles = sum(1 for v in oil_values if v > 10.0)
            ntu_values = [
                r.get("sensors", {}).get("turbidity_ntu")
                for r in telemetry_records
                if isinstance(r.get("sensors", {}).get("turbidity_ntu"), (int, float))
            ]
            if ntu_values:
                avg_ntu = sum(ntu_values) / len(ntu_values)

        summary_data = [
            ["Metric", "Value", "Operational Context"],
            ["Telemetry Samples", str(total_records), "Total periodic cycles analyzed"],
            ["Skimming Duty Cycles", f"{len(skimming_events)} cycles", "Active oil skimmer activations"],
            ["Average Accumulated Volume", f"{avg_vol:.2f} mL", "AVM absolute estimate (trigger: 7200 mL)"],
            ["Peak Collected Oil Mass", f"{max_mass:.1f} g", "Load cell ground truth accumulation"],
            [
                "Average Outlet C_oil",
                f"{avg_oil:.1f} mg/L" if avg_oil is not None else "n/a",
                f"Turbidity-model estimate (baku mutu 10 mg/L); exceeded in {oil_exceed_cycles}/{len(oil_values)} cycles",
            ],
            [
                "Average Outlet NTU",
                f"{avg_ntu:.2f} NTU" if avg_ntu is not None else "n/a",
                "Diagnostic only; NTU model not calibrated",
            ],
            ["Sensor Fault Cycles", str(len(fault_records)), "Fail-safe protective interventions"]
        ]
        t_summary = Table(summary_data, colWidths=[160, 100, 280])
        t_summary.setStyle(TableStyle([
            ('BACKGROUND', (0, 0), (-1, 0), colors.HexColor("#f1f5f9")),
            ('TEXTCOLOR', (0, 0), (-1, 0), colors.HexColor("#0f172a")),
            ('FONTNAME', (0, 0), (-1, 0), 'Helvetica-Bold'),
            ('FONTSIZE', (0, 0), (-1, -1), 8),
            ('BOTTOMPADDING', (0, 0), (-1, -1), 4),
            ('TOPPADDING', (0, 0), (-1, -1), 4),
            ('GRID', (0, 0), (-1, -1), 0.5, colors.HexColor("#cbd5e1")),
        ]))
        elements.append(t_summary)

        # Recent Event History
        elements.append(Spacer(1, 10))
        elements.append(Paragraph("2. System Transition & Safety Event Log", heading2_style))
        if events:
            event_table_data = [["Timestamp (UTC)", "Event", "Severity", "Description"]]
            for ev in events[:12]:
                event_table_data.append([
                    ev.get("timestamp", "")[:19],
                    ev.get("event_type", ""),
                    ev.get("severity", ""),
                    ev.get("description", "")[:45]
                ])
            t_events = Table(event_table_data, colWidths=[110, 110, 60, 260])
            t_events.setStyle(TableStyle([
                ('BACKGROUND', (0, 0), (-1, 0), colors.HexColor("#f1f5f9")),
                ('FONTNAME', (0, 0), (-1, 0), 'Helvetica-Bold'),
                ('FONTSIZE', (0, 0), (-1, -1), 7.5),
                ('BOTTOMPADDING', (0, 0), (-1, -1), 3),
                ('TOPPADDING', (0, 0), (-1, -1), 3),
                ('GRID', (0, 0), (-1, -1), 0.5, colors.HexColor("#e2e8f0")),
            ]))
            elements.append(t_events)
        else:
            elements.append(Paragraph("No anomalous system transitions or fault events recorded during this session.", body_style))

        # Methodology & Limitations
        elements.append(Spacer(1, 10))
        elements.append(Paragraph("3. Engineering Methodology & Scope Limitations", heading2_style))
        elements.append(Paragraph(
            "• <b>HC-SR04 Filtering:</b> 5-sample median window followed by Exponential Moving Average (EMA, alpha=0.20).<br/>"
            "• <b>AVM Volume Estimation:</b> Absolute volume anchored to the calibrated tank geometry (A_eff × alpha_corr × level); activation at 7200 mL (4 cm).<br/>"
            "• <b>Fail-Safe Protection:</b> Solenoid valve is normally-closed (NC), de-energizing to closed state upon any sensor fault.<br/>"
            "• <b>Current Evaluation Status:</b> Phase 1 laptop simulation/mock architecture. Laboratory benchtop validation scheduled for Phase 2.",
            body_style
        ))

        doc.build(elements)
        return str(filepath)
