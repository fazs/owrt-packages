/**
 * @file Connection to the backend: WebSocket /ws (live data) + REST /api (commands, config).
 */

import {
  DEFAULT_HISTORY_SIZE,
  NOTICE_MS,
  SIM_DEFAULT_LOAD_RATIO,
  WS_RETRY_FACTOR,
  WS_RETRY_MAX_MS,
  WS_RETRY_MIN_MS,
} from './constants.js';
import { toG } from './format.js';

/** Length of the trend [s]. */
export const TREND_SECONDS = 30;

/** Reactive state of the application (Svelte 5 `$state`). */
export const live = $state({
  connected: false,
  sample: null,
  config: null,
  description: null, // device description of the terminal (/api/device/description)
  info: null,
  statistics: null,
  measurements: [],
  restartRequired: false,
  lastCapture: null, // {id, at} for the capture animation
  notice: null, // {kind: 'ok'|'error', text}
  calibration: null, // state of the calibration sequence
  unit: readUnit(), // display unit (stored locally), otherwise config.ui.display_unit
  limits: { lo: null, hi: null }, // limits of the distribution [g], null = open; only until reload
});

/**
 * Reads the display unit stored in the browser.
 * @returns {string|null} unit or null (not set or storage not available)
 */
function readUnit() {
  try {
    return localStorage.getItem('ew.unit');
  } catch {
    return null;
  }
}

/**
 * Sets the display unit and stores it in the browser.
 * @param {string} u unit ("mg", "g", "kg", "gr")
 */
export function setUnit(u) {
  live.unit = u;
  try {
    localStorage.setItem('ew.unit', u);
  } catch {
    /* the display unit then only applies until reload */
  }
}

/**
 * Sets the limits of the distribution (apply until the page is reloaded).
 * @param {number|null} lo left limit [g], null = open
 * @param {number|null} hi right limit [g], null = open
 */
export function setLimits(lo, hi) {
  live.limits = { lo, hi };
}

/**
 * Checks whether a value lies outside the limits (the limits themselves belong to it).
 * @param {number} grams value [g]
 * @returns {boolean}
 */
export function outsideLimits(grams) {
  const { lo, hi } = live.limits;
  return (lo != null && grams < lo) || (hi != null && grams > hi);
}

/**
 * Effective display unit: browser, otherwise config.ui.display_unit, otherwise g.
 * @returns {string} unit
 */
export function displayUnit() {
  return live.unit ?? live.config?.ui?.display_unit ?? 'g';
}

/**
 * Trend data (time [s], net value [g] or null for an invalid value); deliberately not
 * deeply reactive (performance), changes are signaled by {@link trendTick}.
 */
export const trend = { t: [], v: [] };
/** Counter that is incremented on every change of {@link trend}. */
export const trendTick = $state({ version: 0 });

let ws = null;
let retryMs = WS_RETRY_MIN_MS;
let noticeTimer = null;

/**
 * Shows a notice for NOTICE_MS.
 * @param {'ok'|'error'} kind kind
 * @param {string} text text
 */
export function notify(kind, text) {
  live.notice = { kind, text };
  clearTimeout(noticeTimer);
  noticeTimer = setTimeout(() => (live.notice = null), NOTICE_MS);
}

/**
 * Appends a value to the trend and discards values older than TREND_SECONDS.
 * @param {number} t time [s]
 * @param {number|null} v net value [g], null for an invalid value
 */
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

/** Signals a change of the trend. */
function trendVersion() {
  trendTick.version++;
}

/**
 * Takes over a complete state (init message or /api/state).
 * @param {object} s snapshot: sample, statistics, measurements, info
 */
function applySnapshot(s) {
  live.sample = s.sample;
  live.calibration = s.sample?.calibration ?? null;
  live.statistics = s.statistics;
  live.measurements = [...(s.measurements ?? [])].reverse();
  live.info = s.info;
  live.restartRequired = !!s.info?.restart_required;
}

/**
 * Number of measurements held in the backend; the snapshot contains only the last SNAPSHOT_MEASUREMENTS.
 * @returns {number}
 */
const historySize = () => live.config?.statistics?.history_size ?? DEFAULT_HISTORY_SIZE;

/** Loads all held measurements via /api/measurements (newest first). */
async function loadMeasurements() {
  try {
    const r = await fetch('/api/measurements');
    if (r.ok) live.measurements = (await r.json()).reverse().slice(0, historySize());
  } catch {
    /* the snapshot remains as the basis */
  }
}

/** Loads the device description of the terminal (once per page load). */
async function loadDescription() {
  try {
    const r = await fetch('/api/device/description');
    if (r.ok) live.description = await r.json();
  } catch {
    /* retry on the next connection */
  }
}

/**
 * Processes a WebSocket message.
 * @param {{type: string}} msg message (init, sample, measurement, measurement_deleted, ...)
 */
function handle(msg) {
  switch (msg.type) {
    case 'init':
      applySnapshot(msg.snapshot);
      live.config = msg.config;
      loadMeasurements();
      if (!live.description) loadDescription();
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
      live.measurements = [msg.measurement, ...live.measurements].slice(0, historySize());
      live.statistics = msg.statistics;
      live.lastCapture = { id: msg.measurement.id, at: performance.now() };
      break;
    case 'measurement_deleted':
      live.measurements = live.measurements.filter((m) => m.id !== msg.id);
      live.statistics = msg.statistics;
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
    case 'device_command':
      if (msg.ok) notify('ok', `Kommando ${msg.id} ausgeführt`);
      else notify('error', `Kommando ${msg.id ?? ''} fehlgeschlagen: ${msg.error}`);
      break;
    case 'device_config':
      if (msg.ok) notify('ok', 'Klemmenparameter übertragen');
      else notify('error', `Klemmenparameter nicht übertragen: ${msg.error}`);
      break;
  }
}

/** Connects the WebSocket; on abort, reconnects with a growing interval (up to WS_RETRY_MAX_MS). */
export function connect() {
  const proto = location.protocol === 'https:' ? 'wss:' : 'ws:';
  ws = new WebSocket(`${proto}//${location.host}/ws`);
  ws.onopen = () => {
    live.connected = true;
    retryMs = WS_RETRY_MIN_MS;
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
    retryMs = Math.min(retryMs * WS_RETRY_FACTOR, WS_RETRY_MAX_MS);
  };
  ws.onerror = () => ws.close();
}

/** Updates live.info via /api/info. */
export async function refreshInfo() {
  try {
    const r = await fetch('/api/info');
    if (r.ok) live.info = await r.json();
  } catch {
    /* the connection is indicated via the WebSocket state */
  }
}

/**
 * POST with an optional JSON body.
 * @param {string} path path
 * @param {object} [body] body
 * @returns {Promise<object>} response
 * @throws {Error} on HTTP error or `ok: false`
 */
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
    /* empty response */
  }
  if (!r.ok || j.ok === false) throw new Error(j.error || `HTTP ${r.status}`);
  return j;
}

/** Sets the tare; result as a notice. */
export async function tare() {
  try {
    await post('/api/tare');
    notify('ok', 'Tara gesetzt');
  } catch (e) {
    notify('error', e.message);
  }
}

/** Clears the tare; result as a notice. */
export async function clearTare() {
  try {
    await post('/api/tare/clear');
    notify('ok', 'Tara gelöscht');
  } catch (e) {
    notify('error', e.message);
  }
}

/**
 * Sends a calibration command; errors as a notice.
 * @param {{action: string}} body command (see README)
 * @returns {Promise<boolean>} true on success
 */
export async function calibrate(body) {
  try {
    await post('/api/calibration', body);
    return true;
  } catch (e) {
    notify('error', e.message);
    return false;
  }
}

/**
 * Simulation only: set the applied mass.
 * @param {number|null} grams mass [g], null = automatic load profile
 */
export async function simLoad(grams) {
  try {
    await post('/api/sim/load', { grams });
  } catch (e) {
    notify('error', e.message);
  }
}

/**
 * Preset of the simulated mass: SIM_DEFAULT_LOAD_RATIO of the nominal load of the terminal.
 * @returns {number|null} mass [g] or null as long as the configuration is missing
 */
export function simDefaultGrams() {
  const dev = live.config?.device;
  const cal = live.config?.calibration;
  return dev && cal ? toG(SIM_DEFAULT_LOAD_RATIO * dev.nominal_load, cal.device_unit) : null;
}

/**
 * GET with a JSON response.
 * @param {string} path path
 * @returns {Promise<object>} response
 * @throws {Error} on HTTP error or `ok: false`
 */
async function getJson(path) {
  const r = await fetch(path);
  let j = {};
  try {
    j = await r.json();
  } catch {
    /* empty response */
  }
  if (!r.ok || j.ok === false) throw new Error(j.error || `HTTP ${r.status}`);
  return j;
}

/** Debug dialog: PDOs, override of the control bits, SDO access, initialization sequence. */
export const debugApi = {
  pdos: () => getJson('/api/debug/pdos'),
  init: () => getJson('/api/debug/init'),
  sdo: (body) => post('/api/debug/sdo', body),
  override: (body) => post('/api/debug/override', body),
  reinit: () => post('/api/debug/reinit'),
  command: (id) => post('/api/device/command', { id }),
  diag: () => getJson('/api/device/diag'),
};

/**
 * Deletes a held measurement; result as a notice.
 * @param {number} id number of the measurement
 */
export async function deleteMeasurement(id) {
  try {
    const r = await fetch(`/api/measurements/${id}`, { method: 'DELETE' });
    const j = await r.json().catch(() => ({}));
    if (!r.ok || j.ok === false) throw new Error(j.error || `HTTP ${r.status}`);
    notify('ok', `Messung ${id} gelöscht`);
  } catch (e) {
    notify('error', `Löschen fehlgeschlagen: ${e.message}`);
  }
}

/** Resets the statistics and deletes all measurements; result as a notice. */
export async function resetStatistics() {
  try {
    await post('/api/statistics/reset');
    notify('ok', 'Statistik zurückgesetzt');
  } catch (e) {
    notify('error', `Zurücksetzen fehlgeschlagen: ${e.message}`);
  }
}

/**
 * Saves the configuration.
 * @param {object} cfg complete configuration
 * @returns {Promise<{ok: boolean, errors?: string[], restart_required?: boolean}>}
 */
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
