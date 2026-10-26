/**
 * @file Mass units and number formatting.
 */

/**
 * Mass units; calculations are done internally in grams. 1 gr = 64.79891 mg (exact).
 * @type {{id: string, label: string, g: number, title?: string}[]}
 */
/** Grams per milligram. */
export const G_PER_MG = 0.001;
/** Grams per gram (base unit). */
export const G_PER_G = 1;
/** Grams per kilogram. */
export const G_PER_KG = 1000;
/** Grams per grain (definition: 1 gr = 64.79891 mg). */
export const G_PER_GR = 0.06479891;

export const UNITS = [
  { id: 'mg', label: 'mg', g: G_PER_MG },
  { id: 'g', label: 'g', g: G_PER_G },
  { id: 'kg', label: 'kg', g: G_PER_KG },
  { id: 'gr', label: 'gr', g: G_PER_GR, title: 'Grain' },
];
const G = Object.fromEntries(UNITS.map((u) => [u.id, u.g]));

/**
 * Converts grams to a unit.
 * @param {number|null|undefined} grams value in grams
 * @param {string} unit target unit; unknown = grams
 * @returns {number|null|undefined} value in `unit`; null/undefined are preserved
 */
export const fromG = (grams, unit) => (grams == null ? grams : grams / (G[unit] ?? G_PER_G));
/**
 * Converts a value to grams.
 * @param {number|null|undefined} value value in `unit`
 * @param {string} unit unit; unknown = grams
 * @returns {number|null|undefined} value in grams; null/undefined are preserved
 */
export const toG = (value, unit) => (value == null ? value : value * (G[unit] ?? G_PER_G));

/** Fixed decimal places per unit. */
const FIXED_DECIMALS = { g: 3, gr: 2 };

/** Decimal places without a known tolerance or default of fmt(). */
export const DEFAULT_DECIMALS = 3;
/** Maximum number of displayed decimal places. */
export const MAX_DECIMALS = 6;

/**
 * Decimal places for the display: fixed per unit, otherwise from the tolerance
 * in the display unit (one digit finer).
 * @param {number} toleranceG tolerance of the measurement [g]
 * @param {string} [unit='g'] display unit
 * @returns {number} decimal places 0..MAX_DECIMALS
 */
export function decimalsFor(toleranceG, unit = 'g') {
  if (unit in FIXED_DECIMALS) return FIXED_DECIMALS[unit];
  const tol = fromG(toleranceG, unit);
  if (!(tol > 0)) return DEFAULT_DECIMALS;
  return Math.min(MAX_DECIMALS, Math.max(0, Math.ceil(-Math.log10(tol)) + 1));
}

/**
 * Formats a number in German style with a fixed number of decimal places.
 * @param {number|null|undefined} v value
 * @param {number} [d=DEFAULT_DECIMALS] decimal places
 * @returns {string} text, „–“ without a value
 */
export function fmt(v, d = DEFAULT_DECIMALS) {
  if (v === null || v === undefined || Number.isNaN(v)) return '–';
  return v.toLocaleString('de-DE', { minimumFractionDigits: d, maximumFractionDigits: d });
}

/**
 * Formats an integer in German style with thousands separators.
 * @param {number|null|undefined} v value
 * @returns {string} text, „–“ without a value
 */
export function fmtInt(v) {
  if (v === null || v === undefined) return '–';
  return v.toLocaleString('de-DE');
}

/** Display texts of the measurement states. */
export const STATE_TEXT = {
  idle: 'Leer',
  settling: 'Einschwingen',
  wait_unload: 'Erfasst, bitte entlasten',
  fault: 'Störung',
};

/** Display texts of the failure reasons (see sample_valid() in the backend). */
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
