/**
 * @file Verbindung zum Backend: WebSocket /ws (Live-Daten) + REST /api (Kommandos, Config).
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

/** Länge des Verlaufs [s]. */
export const TREND_SECONDS = 30;

/** Reaktiver Zustand der Anwendung (Svelte 5 `$state`). */
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
  limits: { lo: null, hi: null }, // Grenzen der Verteilung [g], null = offen; nur bis zum Neuladen
});

/**
 * Liest die im Browser gespeicherte Anzeigeeinheit.
 * @returns {string|null} Einheit oder null (nicht gesetzt bzw. Speicher nicht verfügbar)
 */
function readUnit() {
  try {
    return localStorage.getItem('ew.unit');
  } catch {
    return null;
  }
}

/**
 * Setzt die Anzeigeeinheit und speichert sie im Browser.
 * @param {string} u Einheit ("mg", "g", "kg", "gr")
 */
export function setUnit(u) {
  live.unit = u;
  try {
    localStorage.setItem('ew.unit', u);
  } catch {
    /* Anzeigeeinheit gilt dann nur bis zum Neuladen */
  }
}

/**
 * Setzt die Grenzen der Verteilung (gelten bis zum Neuladen der Seite).
 * @param {number|null} lo linke Grenze [g], null = offen
 * @param {number|null} hi rechte Grenze [g], null = offen
 */
export function setLimits(lo, hi) {
  live.limits = { lo, hi };
}

/**
 * Prüft, ob ein Wert außerhalb der Grenzen liegt (die Grenzen selbst gehören dazu).
 * @param {number} grams Wert [g]
 * @returns {boolean}
 */
export function outsideLimits(grams) {
  const { lo, hi } = live.limits;
  return (lo != null && grams < lo) || (hi != null && grams > hi);
}

/**
 * Wirksame Anzeigeeinheit: Browser, sonst config.ui.display_unit, sonst g.
 * @returns {string} Einheit
 */
export function displayUnit() {
  return live.unit ?? live.config?.ui?.display_unit ?? 'g';
}

/**
 * Trenddaten (Zeit [s], Nettowert [g] bzw. null bei ungültigem Wert); bewusst nicht
 * tief reaktiv (Performance), Änderungen signalisiert {@link trendTick}.
 */
export const trend = { t: [], v: [] };
/** Zähler, der bei jeder Änderung von {@link trend} erhöht wird. */
export const trendTick = $state({ version: 0 });

let ws = null;
let retryMs = WS_RETRY_MIN_MS;
let noticeTimer = null;

/**
 * Zeigt einen Hinweis für NOTICE_MS an.
 * @param {'ok'|'error'} kind Art
 * @param {string} text Text
 */
export function notify(kind, text) {
  live.notice = { kind, text };
  clearTimeout(noticeTimer);
  noticeTimer = setTimeout(() => (live.notice = null), NOTICE_MS);
}

/**
 * Hängt einen Wert an den Verlauf an und verwirft Werte älter als TREND_SECONDS.
 * @param {number} t Zeit [s]
 * @param {number|null} v Nettowert [g], null bei ungültigem Wert
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

/** Signalisiert eine Änderung des Verlaufs. */
function trendVersion() {
  trendTick.version++;
}

/**
 * Übernimmt einen vollständigen Zustand (init-Nachricht bzw. /api/state).
 * @param {object} s Snapshot: sample, statistics, measurements, info
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
 * Anzahl der im Backend gehaltenen Messungen; der Snapshot enthält nur die letzten SNAPSHOT_MEASUREMENTS.
 * @returns {number}
 */
const historySize = () => live.config?.statistics?.history_size ?? DEFAULT_HISTORY_SIZE;

/** Lädt alle gehaltenen Messungen über /api/measurements (neueste zuerst). */
async function loadMeasurements() {
  try {
    const r = await fetch('/api/measurements');
    if (r.ok) live.measurements = (await r.json()).reverse().slice(0, historySize());
  } catch {
    /* Snapshot bleibt als Grundlage */
  }
}

/**
 * Verarbeitet eine WebSocket-Nachricht.
 * @param {{type: string}} msg Nachricht (init, sample, measurement, measurement_deleted, ...)
 */
function handle(msg) {
  switch (msg.type) {
    case 'init':
      applySnapshot(msg.snapshot);
      live.config = msg.config;
      loadMeasurements();
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
    case 'device_config':
      if (msg.ok) notify('ok', 'Klemmenparameter übertragen');
      else notify('error', `Klemmenparameter nicht übertragen: ${msg.error}`);
      break;
  }
}

/** Verbindet den WebSocket; bei Abbruch Wiederverbindung mit wachsendem Abstand (bis WS_RETRY_MAX_MS). */
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

/** Aktualisiert live.info über /api/info. */
export async function refreshInfo() {
  try {
    const r = await fetch('/api/info');
    if (r.ok) live.info = await r.json();
  } catch {
    /* Verbindung wird über den WebSocket-Status angezeigt */
  }
}

/**
 * POST mit optionalem JSON-Körper.
 * @param {string} path Pfad
 * @param {object} [body] Körper
 * @returns {Promise<object>} Antwort
 * @throws {Error} bei HTTP-Fehler oder `ok: false`
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
    /* leere Antwort */
  }
  if (!r.ok || j.ok === false) throw new Error(j.error || `HTTP ${r.status}`);
  return j;
}

/** Setzt Tara; Ergebnis als Hinweis. */
export async function tare() {
  try {
    await post('/api/tare');
    notify('ok', 'Tara gesetzt');
  } catch (e) {
    notify('error', e.message);
  }
}

/** Löscht Tara; Ergebnis als Hinweis. */
export async function clearTare() {
  try {
    await post('/api/tare/clear');
    notify('ok', 'Tara gelöscht');
  } catch (e) {
    notify('error', e.message);
  }
}

/**
 * Sendet ein Kalibrierkommando; Fehler als Hinweis.
 * @param {{action: string}} body Kommando (siehe README)
 * @returns {Promise<boolean>} true bei Erfolg
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
 * Nur Simulation: aufgelegte Masse vorgeben.
 * @param {number|null} grams Masse [g], null = automatisches Lastprofil
 */
export async function simLoad(grams) {
  try {
    await post('/api/sim/load', { grams });
  } catch (e) {
    notify('error', e.message);
  }
}

/**
 * Vorbelegung der simulierten Masse: SIM_DEFAULT_LOAD_RATIO der Nennlast der Klemme.
 * @returns {number|null} Masse [g] oder null, solange die Konfiguration fehlt
 */
export function simDefaultGrams() {
  const dev = live.config?.device;
  const cal = live.config?.calibration;
  return dev && cal ? toG(SIM_DEFAULT_LOAD_RATIO * dev.nominal_load, cal.device_unit) : null;
}

/**
 * GET mit JSON-Antwort.
 * @param {string} path Pfad
 * @returns {Promise<object>} Antwort
 * @throws {Error} bei HTTP-Fehler oder `ok: false`
 */
async function getJson(path) {
  const r = await fetch(path);
  let j = {};
  try {
    j = await r.json();
  } catch {
    /* leere Antwort */
  }
  if (!r.ok || j.ok === false) throw new Error(j.error || `HTTP ${r.status}`);
  return j;
}

/** Debug-Dialog: PDOs, Übersteuerung der Steuerbits, SDO-Zugriff, Initialisierungssequenz. */
export const debugApi = {
  pdos: () => getJson('/api/debug/pdos'),
  init: () => getJson('/api/debug/init'),
  sdo: (body) => post('/api/debug/sdo', body),
  override: (body) => post('/api/debug/override', body),
  reinit: () => post('/api/debug/reinit'),
};

/**
 * Löscht eine gehaltene Messung; Ergebnis als Hinweis.
 * @param {number} id Nummer der Messung
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

/** Setzt die Statistik zurück und löscht alle Messungen; Ergebnis als Hinweis. */
export async function resetStatistics() {
  try {
    await post('/api/statistics/reset');
    notify('ok', 'Statistik zurückgesetzt');
  } catch (e) {
    notify('error', `Zurücksetzen fehlgeschlagen: ${e.message}`);
  }
}

/**
 * Speichert die Konfiguration.
 * @param {object} cfg vollständige Konfiguration
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
