from typing import Dict, Any
from dataclasses import dataclass

@dataclass
class ScenarioProfile:
    name: str
    description: str
    base_surface_height_cm: float
    height_growth_rate_cm_per_cycle: float
    turbidity_base_ntu: float
    turbidity_noise_std: float
    oil_estimated_mg_l: float
    temperature_c: float
    noise_amplitude_cm: float
    spike_probability: float
    sensor_fault_active: bool = False
    emulsion_flag: bool = False

SCENARIO_PROFILES: Dict[str, ScenarioProfile] = {
    "normal": ScenarioProfile(
        name="Normal Operation",
        description="Gradual steady oil accumulation under nominal conditions",
        base_surface_height_cm=3.0,
        height_growth_rate_cm_per_cycle=0.15,
        turbidity_base_ntu=35.0,
        turbidity_noise_std=3.0,
        oil_estimated_mg_l=4.5,
        temperature_c=25.0,
        noise_amplitude_cm=0.08,
        spike_probability=0.02
    ),
    "s1_pure_oil": ScenarioProfile(
        name="S1 - Pure Cooking Oil",
        description="Pure cooking oil, 25 C, moderate flow",
        base_surface_height_cm=4.0,
        height_growth_rate_cm_per_cycle=0.20,
        turbidity_base_ntu=25.0,
        turbidity_noise_std=2.0,
        oil_estimated_mg_l=3.2,
        temperature_c=25.0,
        noise_amplitude_cm=0.05,
        spike_probability=0.01
    ),
    "s2_light_emulsion": ScenarioProfile(
        name="S2 - Light Emulsion",
        description="Light emulsion with 0.1% detergent",
        base_surface_height_cm=3.5,
        height_growth_rate_cm_per_cycle=0.12,
        turbidity_base_ntu=85.0,
        turbidity_noise_std=6.0,
        oil_estimated_mg_l=7.8,
        temperature_c=24.0,
        noise_amplitude_cm=0.10,
        spike_probability=0.05,
        emulsion_flag=True
    ),
    "s3_heavy_emulsion": ScenarioProfile(
        name="S3 - Heavy Emulsion",
        description="Heavy emulsion with 0.5% detergent, high turbidity",
        base_surface_height_cm=3.0,
        height_growth_rate_cm_per_cycle=0.08,
        turbidity_base_ntu=210.0,
        turbidity_noise_std=15.0,
        oil_estimated_mg_l=18.5,
        temperature_c=23.0,
        noise_amplitude_cm=0.15,
        spike_probability=0.08,
        emulsion_flag=True
    ),
    "s4_turbulence": ScenarioProfile(
        name="S4 - High Turbulence",
        description="High inflow and wave action producing ultrasonic spikes",
        base_surface_height_cm=3.8,
        height_growth_rate_cm_per_cycle=0.18,
        turbidity_base_ntu=95.0,
        turbidity_noise_std=12.0,
        oil_estimated_mg_l=8.0,
        temperature_c=24.0,
        noise_amplitude_cm=0.35,
        spike_probability=0.25
    ),
    "s5_thin_layer": ScenarioProfile(
        name="S5 - Thin Oil Layer",
        description="Thin oil layer < 5 mm, near sensor resolution limit",
        base_surface_height_cm=0.4,
        height_growth_rate_cm_per_cycle=0.02,
        turbidity_base_ntu=30.0,
        turbidity_noise_std=2.5,
        oil_estimated_mg_l=4.0,
        temperature_c=25.0,
        noise_amplitude_cm=0.08,
        spike_probability=0.02
    ),
    "s6_low_temp": ScenarioProfile(
        name="S6 - Low Temperature",
        description="Low temperature < 22 C causing high oil viscosity, heater triggered",
        base_surface_height_cm=4.5,
        height_growth_rate_cm_per_cycle=0.15,
        turbidity_base_ntu=45.0,
        turbidity_noise_std=4.0,
        oil_estimated_mg_l=6.0,
        temperature_c=18.5,
        noise_amplitude_cm=0.06,
        spike_probability=0.02
    ),
    "s7_peak_load": ScenarioProfile(
        name="S7 - Peak Load",
        description="Heavy surge inflow with oil layer > 20 mm",
        base_surface_height_cm=6.5,
        height_growth_rate_cm_per_cycle=0.40,
        turbidity_base_ntu=60.0,
        turbidity_noise_std=5.0,
        oil_estimated_mg_l=9.5,
        temperature_c=26.0,
        noise_amplitude_cm=0.12,
        spike_probability=0.04
    ),
    "s8_worst_case": ScenarioProfile(
        name="S8 - Combined Stress Test",
        description="Worst case: Medium emulsion, high turbulence, low temperature",
        base_surface_height_cm=5.0,
        height_growth_rate_cm_per_cycle=0.25,
        turbidity_base_ntu=180.0,
        turbidity_noise_std=20.0,
        oil_estimated_mg_l=15.0,
        temperature_c=19.0,
        noise_amplitude_cm=0.40,
        spike_probability=0.30,
        emulsion_flag=True
    ),
    "sensor_fault": ScenarioProfile(
        name="Sensor Fault Simulation",
        description="Simulates ultrasonic out-of-range and turbidity disconnection",
        base_surface_height_cm=0.0,
        height_growth_rate_cm_per_cycle=0.0,
        turbidity_base_ntu=0.0,
        turbidity_noise_std=0.0,
        oil_estimated_mg_l=0.0,
        temperature_c=25.0,
        noise_amplitude_cm=0.0,
        spike_probability=0.0,
        sensor_fault_active=True
    )
}
