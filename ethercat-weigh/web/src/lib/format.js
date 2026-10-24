/**
 * @file Masseneinheiten und Zahlenformatierung.
 */

/**
 * Masseneinheiten; intern wird in Gramm gerechnet. 1 gr = 64,79891 mg (exakt).
 * @type {{id: string, label: string, g: number, title?: string}[]}
 */
/** Gramm je Milligramm. */
export const G_PER_MG = 0.001;
/** Gramm je Gramm (Basiseinheit). */
export const G_PER_G = 1;
/** Gramm je Kilogramm. */
export const G_PER_KG = 1000;
/** Gramm je Grain (Definition: 1 gr = 64,79891 mg). */
export const G_PER_GR = 0.06479891;

export const UNITS = [
  { id: 'mg', label: 'mg', g: G_PER_MG },
  { id: 'g', label: 'g', g: G_PER_G },
  { id: 'kg', label: 'kg', g: G_PER_KG },
  { id: 'gr', label: 'gr', g: G_PER_GR, title: 'Grain' },
];
const G = Object.fromEntries(UNITS.map((u) => [u.id, u.g]));

/**
 * Rechnet Gramm in eine Einheit um.
 * @param {number|null|undefined} grams Wert in Gramm
 * @param {string} unit Zieleinheit; unbekannt = Gramm
 * @returns {number|null|undefined} Wert in `unit`; null/undefined bleiben erhalten
 */
export const fromG = (grams, unit) => (grams == null ? grams : grams / (G[unit] ?? G_PER_G));
/**
 * Rechnet einen Wert in Gramm um.
 * @param {number|null|undefined} value Wert in `unit`
 * @param {string} unit Einheit; unbekannt = Gramm
 * @returns {number|null|undefined} Wert in Gramm; null/undefined bleiben erhalten
 */
export const toG = (value, unit) => (value == null ? value : value * (G[unit] ?? G_PER_G));

/** Feste Nachkommastellen je Einheit. */
const FIXED_DECIMALS = { g: 3, gr: 2 };

/** Nachkommastellen ohne bekannte Toleranz bzw. Vorgabe von fmt(). */
export const DEFAULT_DECIMALS = 3;
/** Höchstens angezeigte Nachkommastellen. */
export const MAX_DECIMALS = 6;

/**
 * Nachkommastellen für die Anzeige: fest je Einheit, sonst aus der Toleranz
 * in der Anzeigeeinheit (eine Stelle feiner).
 * @param {number} toleranceG Toleranz der Messung [g]
 * @param {string} [unit='g'] Anzeigeeinheit
 * @returns {number} Nachkommastellen 0..MAX_DECIMALS
 */
export function decimalsFor(toleranceG, unit = 'g') {
  if (unit in FIXED_DECIMALS) return FIXED_DECIMALS[unit];
  const tol = fromG(toleranceG, unit);
  if (!(tol > 0)) return DEFAULT_DECIMALS;
  return Math.min(MAX_DECIMALS, Math.max(0, Math.ceil(-Math.log10(tol)) + 1));
}

/**
 * Formatiert eine Zahl deutsch mit fester Anzahl Nachkommastellen.
 * @param {number|null|undefined} v Wert
 * @param {number} [d=DEFAULT_DECIMALS] Nachkommastellen
 * @returns {string} Text, „–“ ohne Wert
 */
export function fmt(v, d = DEFAULT_DECIMALS) {
  if (v === null || v === undefined || Number.isNaN(v)) return '–';
  return v.toLocaleString('de-DE', { minimumFractionDigits: d, maximumFractionDigits: d });
}

/**
 * Formatiert eine Ganzzahl deutsch mit Tausenderpunkten.
 * @param {number|null|undefined} v Wert
 * @returns {string} Text, „–“ ohne Wert
 */
export function fmtInt(v) {
  if (v === null || v === undefined) return '–';
  return v.toLocaleString('de-DE');
}

/** Anzeigetexte der Messzustände. */
export const STATE_TEXT = {
  idle: 'Leer',
  settling: 'Einschwingen',
  wait_unload: 'Erfasst, bitte entlasten',
  fault: 'Störung',
};

/** Anzeigetexte der Fehlergründe (siehe sample_valid() im Backend). */
export const FAULT_TEXT = {
  link: 'Keine Verbindung zur Klemme',
  wkc: 'Working Counter fehlerhaft',
  error: 'Klemme meldet Fehler',
  overrange: 'Messbereich überschritten',
  data_invalid: 'Messdaten ungültig',
  calibration: 'Selbstkalibrierung der Klemme läuft',
  calibrating: 'Kalibrierung aktiv',
  uncalibrated: 'Keine gültige Kennlinie',
};
