const API_BASE = window.location.origin;
const OIL_LIMIT_MGL = 10.0;
const OIL_LOW_MGL = 1.0;
const DEFAULT_CRITICAL_VOLUME_ML = 7200;
const MAX_DATA_POINTS = 30;

const STATUS_META = {
  NORMAL: { label: 'Normal', state: 'ok' },
  SKIMMING_AKTIF: { label: 'Skimming active', state: 'active' },
  EMULSI_TERDETEKSI: { label: 'Emulsion detected', state: 'warning' },
  PENUH: { label: 'Full', state: 'danger' },
  GANGGUAN_SENSOR: { label: 'Sensor fault', state: 'danger' },
  AWAITING_TELEMETRY: { label: 'Awaiting telemetry', state: 'unknown' },
  UNKNOWN: { label: 'Unknown', state: 'unknown' },
};

let chartUltrasonic = null;
let chartVolume = null;
let chartOil = null;
let chartsReady = false;

let criticalVolumeMl = DEFAULT_CRITICAL_VOLUME_ML;
let lastTelemetryTimestamp = null;
let lastFetchOk = false;
let toastTimer = null;
let manualDischargeOpen = false;

const $ = (id) => document.getElementById(id);

function toNumber(value) {
  if (value === null || value === undefined || value === '') return null;
  const number = Number(value);
  return Number.isFinite(number) ? number : null;
}

function parseDate(value) {
  if (!value) return null;
  const date = new Date(value);
  return Number.isNaN(date.getTime()) ? null : date;
}

function pad(value) {
  return String(value).padStart(2, '0');
}

function formatTime(value) {
  const date = parseDate(value);
  if (!date) return '--';
  return `${pad(date.getHours())}:${pad(date.getMinutes())}:${pad(date.getSeconds())}`;
}

function formatEventTime(value) {
  const date = parseDate(value);
  if (!date) return '--';
  return `${pad(date.getMonth() + 1)}-${pad(date.getDate())} ${formatTime(date)}`;
}

function formatInteger(value) {
  return new Intl.NumberFormat('en-US', { maximumFractionDigits: 0 }).format(value);
}

function escapeHtml(value) {
  return String(value ?? '').replace(/[&<>"']/g, (character) => ({
    '&': '&amp;',
    '<': '&lt;',
    '>': '&gt;',
    '"': '&quot;',
    "'": '&#39;',
  }[character]));
}

function setText(id, text) {
  const element = $(id);
  if (element) element.textContent = text;
}

function setButtonLabel(button, text) {
  const label = button.querySelector('.btn-label');
  if (label) {
    label.textContent = text;
  } else {
    button.textContent = text;
  }
}

function showToast(message, kind = 'info') {
  const toast = $('toast');
  if (!toast) return;

  toast.textContent = String(message);
  toast.dataset.kind = kind;
  toast.classList.add('is-visible');

  window.clearTimeout(toastTimer);
  toastTimer = window.setTimeout(() => {
    toast.classList.remove('is-visible');
  }, 4500);
}

window.showToast = showToast;

function buildChartOptions() {
  return {
    responsive: true,
    maintainAspectRatio: false,
    animation: false,
    interaction: { mode: 'index', intersect: false },
    layout: { padding: { top: 2, right: 2, bottom: 0, left: 0 } },
    plugins: {
      legend: {
        position: 'bottom',
        align: 'start',
        labels: {
          color: '#93a6b1',
          boxWidth: 8,
          boxHeight: 8,
          usePointStyle: true,
          pointStyle: 'line',
          padding: 12,
          font: { size: 9.5 },
        },
      },
      tooltip: {
        backgroundColor: '#0b1115',
        borderColor: '#22323b',
        borderWidth: 1,
        titleColor: '#f0f7f9',
        bodyColor: '#c5d3da',
        padding: 10,
        cornerRadius: 6,
        boxWidth: 8,
        boxHeight: 8,
        usePointStyle: true,
      },
    },
    scales: {
      x: {
        border: { display: false },
        grid: { display: false },
        ticks: {
          color: '#647984',
          maxRotation: 0,
          autoSkip: true,
          maxTicksLimit: 6,
          font: { size: 9 },
        },
      },
      y: {
        beginAtZero: true,
        border: { display: false },
        grid: { color: 'rgba(147, 166, 177, 0.1)', drawTicks: false },
        ticks: {
          color: '#647984',
          maxTicksLimit: 5,
          padding: 6,
          font: { size: 9 },
        },
      },
    },
  };
}

function initCharts() {
  if (typeof Chart === 'undefined') {
    showToast('Chart library could not be loaded. Metrics will continue to update.', 'error');
    return;
  }

  Chart.defaults.color = '#93a6b1';
  Chart.defaults.font.family = 'ui-sans-serif, system-ui, -apple-system, "Segoe UI", sans-serif';
  Chart.defaults.font.size = 10;

  chartUltrasonic = new Chart($('chart-ultrasonic').getContext('2d'), {
    type: 'line',
    data: {
      labels: [],
      datasets: [
        { label: 'Raw', data: [], borderColor: '#f27d8b', borderWidth: 1, pointRadius: 0, fill: false, tension: 0.15 },
        { label: 'Median', data: [], borderColor: '#f0b35a', borderWidth: 1.5, pointRadius: 0, fill: false, tension: 0.2 },
        { label: 'EMA filtered', data: [], borderColor: '#58c8bd', borderWidth: 2, pointRadius: 0, fill: false, tension: 0.2 },
      ],
    },
    options: buildChartOptions(),
  });

  chartVolume = new Chart($('chart-volume').getContext('2d'), {
    type: 'line',
    data: {
      labels: [],
      datasets: [
        {
          label: 'AVM volume (mL)',
          data: [],
          borderColor: '#58c8bd',
          backgroundColor: 'rgba(88, 200, 189, 0.08)',
          borderWidth: 2,
          pointRadius: 0,
          fill: true,
          tension: 0.2,
        },
        {
          label: `Threshold (${formatInteger(criticalVolumeMl)} mL)`,
          data: [],
          borderColor: '#f0b35a',
          borderDash: [5, 4],
          borderWidth: 1.5,
          pointRadius: 0,
          fill: false,
        },
      ],
    },
    options: buildChartOptions(),
  });

  chartOil = new Chart($('chart-oil').getContext('2d'), {
    type: 'line',
    data: {
      labels: [],
      datasets: [
        { label: 'C_oil (mg/L)', data: [], borderColor: '#5bd6b0', borderWidth: 2, pointRadius: 0, fill: false, spanGaps: false },
        { label: 'Discharge limit (10 mg/L)', data: [], borderColor: '#f27d8b', borderDash: [5, 4], borderWidth: 1.5, pointRadius: 0, fill: false },
      ],
    },
    options: buildChartOptions(),
  });

  chartsReady = true;
}

function setConnection(state, label) {
  const badge = $('connection-badge');
  if (badge) badge.dataset.state = state;
  setText('connection-status', label);
}

function updateStatusBadge(status) {
  const badge = $('badge-status');
  if (!badge) return;

  const key = String(status || 'UNKNOWN').trim().toUpperCase().replace(/[\s-]+/g, '_');
  const meta = STATUS_META[key] || { label: key.replace(/_/g, ' '), state: 'unknown' };

  badge.textContent = meta.label;
  badge.dataset.state = meta.state;
}

function updateCriticalVolume(value) {
  const critical = toNumber(value);
  if (critical === null || critical <= 0 || critical === criticalVolumeMl) return;

  criticalVolumeMl = critical;
  setText('val-volume-limit', formatInteger(critical));

  const track = $('bar-volume')?.parentElement;
  if (track) track.setAttribute('aria-valuemax', String(critical));

  if (chartVolume) {
    chartVolume.data.datasets[1].label = `Threshold (${formatInteger(critical)} mL)`;
    chartVolume.update('none');
  }
}

function updateVolume(value, criticalValue) {
  const volume = toNumber(value) ?? 0;
  const critical = toNumber(criticalValue) > 0 ? toNumber(criticalValue) : criticalVolumeMl;
  const percentage = critical > 0 ? Math.max(0, Math.min(100, (volume / critical) * 100)) : 0;

  setText('val-volume', volume.toFixed(1));
  setText('val-volume-pct', `${Math.round(percentage)}%`);

  const fill = $('bar-volume');
  if (!fill) return;

  fill.style.width = `${percentage}%`;
  fill.dataset.state = volume >= critical ? 'danger' : percentage >= 80 ? 'warning' : 'ok';

  const track = fill.parentElement;
  if (track) {
    track.setAttribute('aria-valuenow', volume.toFixed(1));
    track.setAttribute('aria-valuetext', `${volume.toFixed(1)} of ${formatInteger(critical)} mL`);
  }
}

function updateOil(value) {
  const oil = toNumber(value);
  const valueElement = $('val-oil-conc');
  const statusElement = $('val-oil-status');
  if (!valueElement || !statusElement) return;

  if (oil === null) {
    valueElement.textContent = '—';
    valueElement.dataset.state = 'unknown';
    statusElement.textContent = 'UNKNOWN';
    statusElement.dataset.state = 'unknown';
    return;
  }

  const low = oil <= OIL_LOW_MGL;
  const safe = oil <= OIL_LIMIT_MGL;
  valueElement.textContent = Math.max(0, oil).toFixed(1);
  valueElement.dataset.state = safe ? 'ok' : 'danger';
  statusElement.textContent = low ? 'C_oil rendah' : safe ? 'SAFE' : 'OVER LIMIT';
  statusElement.dataset.state = safe ? 'ok' : 'danger';
}

function updateActuators(actuators) {
  if (!actuators) return;

  const pwmElement = $('val-pwm');
  const solenoidElement = $('val-solenoid');
  const heaterElement = $('val-heater');
  const pwm = toNumber(actuators.motor_pwm) ?? 0;
  const solenoid = String(actuators.solenoid_state || 'CLOSED').toUpperCase();
  const heater = String(actuators.heater_state || 'OFF').toUpperCase();

  if (pwmElement) {
    pwmElement.textContent = String(pwm);
    pwmElement.dataset.state = pwm > 0 ? 'active' : 'idle';
  }
  if (solenoidElement) {
    solenoidElement.textContent = solenoid;
    solenoidElement.dataset.state = solenoid === 'OPEN' ? 'active' : 'idle';
  }
  if (heaterElement) {
    heaterElement.textContent = heater;
    heaterElement.dataset.state = heater === 'ON' ? 'active' : 'idle';
  }

  manualDischargeOpen = solenoid === 'OPEN';
  updateManualDischargeButton();
}

function updateManualDischargeButton() {
  const button = $('btn-manual-discharge');
  if (!button) return;

  setButtonLabel(button, manualDischargeOpen ? 'Close valve + pump' : 'Open valve + pump');
  button.dataset.state = manualDischargeOpen ? 'active' : 'idle';
  button.setAttribute('aria-pressed', manualDischargeOpen ? 'true' : 'false');

  const hint = $('manual-discharge-hint');
  if (hint) {
    hint.textContent = manualDischargeOpen
      ? 'Discharge path OPEN. Close to return to automatic control.'
      : 'Manual override: valve + pump only; C_oil/volume bypassed.';
  }
}

function setupManualDischargeButton() {
  const button = $('btn-manual-discharge');
  if (!button) return;

  button.addEventListener('click', async () => {
    if (button.disabled) return;

    const nextState = manualDischargeOpen ? 'close' : 'open';
    const message = nextState === 'open'
      ? 'Buka valve + pompa discharge secara manual? C_oil/volume/FSM di-bypass sampai ditutup.'
      : 'Tutup valve + pompa discharge?';
    if (!window.confirm(message)) return;

    button.disabled = true;
    setButtonLabel(button, 'Sending...');
    try {
      const response = await fetch(`${API_BASE}/api/actuators/manual-discharge`, {
        method: 'POST',
        headers: { 'Content-Type': 'application/json' },
        body: JSON.stringify({ state: nextState }),
      });
      const data = await response.json().catch(() => ({}));
      if (!response.ok) {
        throw new Error(data.detail || `HTTP ${response.status}`);
      }

      manualDischargeOpen = nextState === 'open';
      showToast(
        nextState === 'open'
          ? 'Manual discharge OPEN sent.'
          : 'Manual discharge CLOSE sent.',
        'success',
      );
    } catch (error) {
      showToast(`Manual discharge failed: ${error.message}`, 'error');
    } finally {
      button.disabled = false;
      updateManualDischargeButton();
    }
  });

  updateManualDischargeButton();
}

function checkStaleData() {
  if (!lastFetchOk) return;

  if (!lastTelemetryTimestamp) {
    setConnection('waiting', 'Awaiting telemetry');
    return;
  }

  const ageMs = Date.now() - lastTelemetryTimestamp.getTime();
  if (ageMs > 45000) {
    setConnection('stale', 'Stale data · 45s+');
  } else {
    setConnection('live', 'Live telemetry');
  }
}

async function fetchStatus() {
  try {
    const response = await fetch(`${API_BASE}/api/status`, { cache: 'no-store' });
    if (!response.ok) throw new Error(`HTTP ${response.status}`);
    const data = await response.json();

    lastFetchOk = true;

    if (data.status === 'AWAITING_TELEMETRY') {
      updateStatusBadge('AWAITING_TELEMETRY');
      setText('val-fsm-state', '--');
      lastTelemetryTimestamp = null;
      setConnection('waiting', 'Awaiting telemetry');
      return;
    }

    updateStatusBadge(data.status);
    setText('val-fsm-state', data.embedded_state || '--');

    if (data.model) {
      updateCriticalVolume(data.model.critical_volume_ml);
      updateVolume(data.model.accumulated_volume_ml, data.model.critical_volume_ml);
    }

    if (data.sensors) {
      updateOil(data.sensors.oil_estimated_mg_l);
      const turbidity = toNumber(data.sensors.turbidity_ntu);
      setText('val-turbidity', turbidity === null ? '—' : turbidity.toFixed(1));

      const mass = toNumber(data.sensors.oil_mass_g) ?? 0;
      const containerElement = $('val-container-status');
      const nearFullThreshold = toNumber(data.thresholds?.container_near_full_g) ?? 4000;
      const nearFull = mass >= nearFullThreshold;
      setText('val-mass', mass.toFixed(1));
      if (containerElement) {
        containerElement.textContent = nearFull ? 'NEAR FULL' : 'NOMINAL';
        containerElement.dataset.state = nearFull ? 'warning' : 'ok';
      }
    }

    updateActuators(data.actuators);

    const timestamp = parseDate(data.timestamp);
    lastTelemetryTimestamp = timestamp;
    setText('val-last-update', timestamp ? `Last update: ${formatTime(timestamp)}` : 'Last update: --');

    checkStaleData();
  } catch (error) {
    lastFetchOk = false;
    setConnection('error', 'Backend disconnected');
    console.warn('Status fetch failed:', error);
  }
}

async function fetchHistory() {
  if (!chartsReady) return;

  try {
    const response = await fetch(`${API_BASE}/api/telemetry/history?limit=${MAX_DATA_POINTS}`, { cache: 'no-store' });
    if (!response.ok) return;
    const json = await response.json();
    const records = json.data || [];

    const labels = [];
    const rawUltrasonic = [];
    const medianUltrasonic = [];
    const filteredUltrasonic = [];
    const volume = [];
    const criticalVolume = [];
    const oil = [];
    const oilLimit = [];

    records.forEach((record) => {
      const sensors = record.sensors || {};
      const model = record.model || {};

      labels.push(formatTime(record.timestamp));
      rawUltrasonic.push(toNumber(sensors.ultrasonic_raw));
      medianUltrasonic.push(toNumber(sensors.ultrasonic_median));
      filteredUltrasonic.push(toNumber(sensors.ultrasonic_filtered));
      volume.push(toNumber(model.accumulated_volume_ml));
      criticalVolume.push(toNumber(model.critical_volume_ml) ?? criticalVolumeMl);
      const oilValue = toNumber(sensors.oil_estimated_mg_l);
      oil.push(oilValue === null ? null : Math.max(0, oilValue));
      oilLimit.push(OIL_LIMIT_MGL);
    });

    chartUltrasonic.data.labels = labels;
    chartUltrasonic.data.datasets[0].data = rawUltrasonic;
    chartUltrasonic.data.datasets[1].data = medianUltrasonic;
    chartUltrasonic.data.datasets[2].data = filteredUltrasonic;
    chartUltrasonic.update('none');

    chartVolume.data.labels = labels;
    chartVolume.data.datasets[0].data = volume;
    chartVolume.data.datasets[1].data = criticalVolume;
    chartVolume.update('none');

    chartOil.data.labels = labels;
    chartOil.data.datasets[0].data = oil;
    chartOil.data.datasets[1].data = oilLimit;
    chartOil.update('none');
  } catch (error) {
    console.warn('History fetch failed:', error);
  }
}

async function fetchEvents() {
  const list = $('event-log-body');
  if (!list) return;

  try {
    const response = await fetch(`${API_BASE}/api/events?limit=15`, { cache: 'no-store' });
    if (!response.ok) return;
    const json = await response.json();
    const events = json.data || [];

    if (events.length === 0) {
      list.innerHTML = '<p class="empty-state">No events logged yet.</p>';
      return;
    }

    list.innerHTML = events.map((event) => {
      const severity = String(event.severity || 'INFO').toUpperCase();
      const severityState = severity === 'ERROR' ? 'error' : severity === 'WARNING' ? 'warning' : severity === 'INFO' ? 'info' : 'neutral';
      const timestamp = parseDate(event.timestamp);
      const timestampLabel = formatEventTime(event.timestamp);
      const timestampAttribute = timestamp ? ` datetime="${timestamp.toISOString()}"` : '';
      const deviceId = event.device_id || '--';

      return `
        <article class="event-item" role="listitem" data-severity="${severityState}">
          <div class="event-when">
            <time${timestampAttribute}>${escapeHtml(timestampLabel)}</time>
            <span class="event-device" title="${escapeHtml(deviceId)}">${escapeHtml(deviceId)}</span>
          </div>
          <div class="event-main">
            <span class="event-type">${escapeHtml(event.event_type || 'EVENT')}</span>
            <p class="event-desc">${escapeHtml(event.description || 'No description')}</p>
          </div>
          <span class="severity" data-severity="${severityState}">${escapeHtml(severity)}</span>
        </article>
      `;
    }).join('');
  } catch (error) {
    console.warn('Event fetch failed:', error);
  }
}

function setupReportButton() {
  const button = $('btn-generate-report');
  if (!button) return;

  button.addEventListener('click', async () => {
    if (button.disabled) return;

    button.disabled = true;
    button.setAttribute('aria-busy', 'true');
    setButtonLabel(button, 'Generating…');

    try {
      const response = await fetch(`${API_BASE}/api/reports/generate`, {
        method: 'POST',
        headers: { 'Content-Type': 'application/json' },
        body: JSON.stringify({ scenario_name: 'Phase 1 Laptop Simulation', device_id: 'SGT-001' }),
      });

      if (!response.ok) {
        let detail = `HTTP ${response.status}`;
        try {
          const body = await response.json();
          if (body.detail) detail = body.detail;
        } catch {
          // Keep the HTTP status as the error message.
        }
        throw new Error(detail);
      }

      const data = await response.json();
      if (!data.download_url) throw new Error('Download URL missing');

      const downloadUrl = `${API_BASE}${data.download_url}`;
      const opened = window.open(downloadUrl, '_blank');
      if (opened) {
        opened.opener = null;
        showToast('Report generated. The PDF opened in a new tab.', 'success');
      } else {
        window.location.assign(downloadUrl);
        showToast('Report generated. Starting download…', 'success');
      }
    } catch (error) {
      console.error('Report generation failed:', error);
      showToast(`Report generation failed: ${error.message}`, 'error');
    } finally {
      button.disabled = false;
      button.removeAttribute('aria-busy');
      setButtonLabel(button, 'Generate report');
    }
  });
}

window.addEventListener('DOMContentLoaded', () => {
  initCharts();
  setupReportButton();
  setupManualDischargeButton();

  fetchStatus();
  fetchHistory();
  fetchEvents();

  window.setInterval(fetchStatus, 2000);
  window.setInterval(fetchHistory, 3000);
  window.setInterval(fetchEvents, 5000);
  window.setInterval(checkStaleData, 5000);
});
