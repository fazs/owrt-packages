// Verbindung zum Backend: WebSocket /ws (Live-Daten) + REST /api (Kommandos, Config)

export const TREND_SECONDS = 30;

export const live = $state({
  connected: false,
  sample: null,
  config: null,
  info: null,
  statistics: null,
  measurements: [],
  restartRequired: false,
  lastCapture: null, // {id, at} für die Erfassungsanimation
  notice: null, // {kind: 'ok'|'error', text}
  calibration: null, // Status des Kalibrierablaufs
  unit: readUnit(), // Anzeigeeinheit (lokal gespeichert), sonst config.ui.display_unit
});

function readUnit() {
  try {
    return localStorage.getItem('ew.unit');
  } catch {
    return null;
  }
}

export function setUnit(u) {
  live.unit = u;
  try {
    localStorage.setItem('ew.unit', u);
  } catch {
    /* Anzeigeeinheit gilt dann nur bis zum Neuladen */
  }
}

// Wirksame Anzeigeeinheit
export function displayUnit() {
  return live.unit ?? live.config?.ui?.display_unit ?? 'g';
}

// Trenddaten bewusst nicht tief reaktiv (Performance); trendVersion signalisiert Änderungen
export const trend = { t: [], v: [] };
export const trendTick = $state({ version: 0 });

let ws = null;
let retryMs = 1000;
let noticeTimer = null;

export function notify(kind, text) {
  live.notice = { kind, text };
  clearTimeout(noticeTimer);
  noticeTimer = setTimeout(() => (live.notice = null), 4000);
}

function pushTrend(t, v) {
  trend.t.push(t);
  trend.v.push(v);
  const cutoff = t - TREND_SECONDS;
  let drop = 0;
  while (drop < trend.t.length && trend.t[drop] < cutoff) drop++;
  if (drop > 0) {
    trend.t.splice(0, drop);
    trend.v.splice(0, drop);
  }
  trendVersion();
}

function trendVersion() {
  trendTick.version++;
}

function applySnapshot(s) {
  live.sample = s.sample;
  live.calibration = s.sample?.calibration ?? null;
  live.statistics = s.statistics;
  live.measurements = [...(s.measurements ?? [])].reverse();
  live.info = s.info;
  live.restartRequired = !!s.info?.restart_required;
}

function handle(msg) {
  switch (msg.type) {
    case 'hello':
      applySnapshot(msg.snapshot);
      live.config = msg.config;
      trend.t.length = 0;
      trend.v.length = 0;
      trendVersion();
      break;
    case 'sample':
      live.sample = msg;
      live.calibration = msg.calibration ?? null;
      pushTrend(msg.t, msg.valid ? msg.value : null);
      break;
    case 'measurement':
      live.measurements = [msg.measurement, ...live.measurements].slice(0, 200);
      live.statistics = msg.statistics;
      live.lastCapture = { id: msg.measurement.id, at: performance.now() };
      break;
    case 'statistics_reset':
      live.statistics = msg.statistics;
      live.measurements = [];
      break;
    case 'config':
      live.config = msg.config;
      live.restartRequired = live.restartRequired || msg.restart_required;
      break;
    case 'backend':
      if (live.info) live.info = { ...live.info, backend_state: msg.state, backend_error: msg.error };
      break;
    case 'calibration':
      live.calibration = msg.calibration;
      break;
    case 'device_config':
      if (msg.ok) notify('ok', 'Klemmenparameter übertragen');
      else notify('error', `Klemmenparameter nicht übertragen: ${msg.error}`);
      break;
  }
}

export function connect() {
  const proto = location.protocol === 'https:' ? 'wss:' : 'ws:';
  ws = new WebSocket(`${proto}//${location.host}/ws`);
  ws.onopen = () => {
    live.connected = true;
    retryMs = 1000;
  };
  ws.onmessage = (ev) => {
    try {
      handle(JSON.parse(ev.data));
    } catch (e) {
      console.error('WebSocket-Nachricht verworfen', e);
    }
  };
  ws.onclose = () => {
    live.connected = false;
    setTimeout(connect, retryMs);
    retryMs = Math.min(retryMs * 2, 8000);
  };
  ws.onerror = () => ws.close();
}

export async function refreshInfo() {
  try {
    const r = await fetch('/api/info');
    if (r.ok) live.info = await r.json();
  } catch {
    /* Verbindung wird über den WebSocket-Status angezeigt */
  }
}

async function post(path, body) {
  const r = await fetch(path, {
    method: 'POST',
    headers: body ? { 'Content-Type': 'application/json' } : {},
    body: body ? JSON.stringify(body) : undefined,
  });
  let j = {};
  try {
    j = await r.json();
  } catch {
    /* leere Antwort */
  }
  if (!r.ok || j.ok === false) throw new Error(j.error || `HTTP ${r.status}`);
  return j;
}

export async function tare() {
  try {
    await post('/api/tare');
    notify('ok', 'Tara gesetzt');
  } catch (e) {
    notify('error', e.message);
  }
}

export async function clearTare() {
  try {
    await post('/api/tare/clear');
    notify('ok', 'Tara gelöscht');
  } catch (e) {
    notify('error', e.message);
  }
}

// Kalibrierkommando; liefert true bei Erfolg, Fehler als Hinweis
export async function calibrate(body) {
  try {
    await post('/api/calibration', body);
    return true;
  } catch (e) {
    notify('error', e.message);
    return false;
  }
}

export async function simLoad(grams) {
  try {
    await post('/api/sim/load', { grams });
  } catch (e) {
    notify('error', e.message);
  }
}

export async function resetStatistics() {
  try {
    await post('/api/statistics/reset');
    notify('ok', 'Statistik zurückgesetzt');
  } catch (e) {
    notify('error', `Zurücksetzen fehlgeschlagen: ${e.message}`);
  }
}

// Liefert {ok, errors, restart_required}
export async function saveConfig(cfg) {
  const r = await fetch('/api/config', {
    method: 'PUT',
    headers: { 'Content-Type': 'application/json' },
    body: JSON.stringify(cfg),
  });
  let body;
  try {
    body = await r.json();
  } catch {
    body = { ok: false, errors: [`HTTP ${r.status}`] };
  }
  return body;
}
