// Masseneinheiten; intern wird in Gramm gerechnet. 1 gr = 64,79891 mg (exakt).
export const UNITS = [
  { id: 'mg', label: 'mg', g: 0.001 },
  { id: 'g', label: 'g', g: 1 },
  { id: 'kg', label: 'kg', g: 1000 },
  { id: 'gr', label: 'gr', g: 0.06479891, title: 'Grain' },
];
const G = Object.fromEntries(UNITS.map((u) => [u.id, u.g]));

export const fromG = (grams, unit) => (grams == null ? grams : grams / (G[unit] ?? 1));
export const toG = (value, unit) => (value == null ? value : value * (G[unit] ?? 1));

// Nachkommastellen aus der Toleranz [g] in der Anzeigeeinheit (eine Stelle feiner)
export function decimalsFor(toleranceG, unit = 'g') {
  const tol = fromG(toleranceG, unit);
  if (!(tol > 0)) return 3;
  return Math.min(6, Math.max(0, Math.ceil(-Math.log10(tol)) + 1));
}

export function fmt(v, d = 3) {
  if (v === null || v === undefined || Number.isNaN(v)) return '–';
  return v.toLocaleString('de-DE', { minimumFractionDigits: d, maximumFractionDigits: d });
}

export function fmtInt(v) {
  if (v === null || v === undefined) return '–';
  return v.toLocaleString('de-DE');
}

export const STATE_TEXT = {
  idle: 'Leer',
  settling: 'Einschwingen',
  wait_unload: 'Erfasst, bitte entlasten',
  fault: 'Störung',
};

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
