// CoE-Objekte der EL3356: Adressformat, Wertebereiche (Enums) und Eingabe-Parsing

export const hex = (v, digits) => '0x' + Number(v).toString(16).toUpperCase().padStart(digits, '0');
export const objKey = (index, sub) => `${hex(index, 4)}:${Number(sub).toString(16).toUpperCase().padStart(2, '0')}`;

// 0x8000:11/:12 Mode0/Mode1 filter settings (ESI: DT0800EN16/DT0801EN16)
export const FILTER_OPTIONS = [
  [0, 'FIR 50 Hz'],
  [1, 'FIR 60 Hz'],
  [2, 'IIR1'],
  [3, 'IIR2'],
  [4, 'IIR3'],
  [5, 'IIR4'],
  [6, 'IIR5'],
  [7, 'IIR6'],
  [8, 'IIR7'],
  [9, 'IIR8'],
  [10, 'Dynamic IIR'],
  [11, 'PDO Filter frequency (0x7000:11)'],
];

const BOOL = [
  [0, 'FALSE'],
  [1, 'TRUE'],
];

// Feste Wertebereiche je Objekt; PDO-Zuordnung (0x1C12/0x1C13) ergibt sich aus dem Katalog
const ENUMS = {
  '0x7000:04': [
    [0, 'Mode 0'],
    [1, 'Mode 1'],
  ],
  '0x8000:11': FILTER_OPTIONS,
  '0x8000:12': FILTER_OPTIONS,
  '0xFB00:01': [
    [0x0000, '0x0000 Kein Kommando'],
    [0x0101, '0x0101 Nullabgleich'],
    [0x0102, '0x0102 Kalibrierung mit Reference load'],
  ],
};

// Auswahlliste für ein Objekt oder null (freie Eingabe)
export function enumFor(index, sub, type, pdos = []) {
  if ((index === 0x1c12 || index === 0x1c13) && sub > 0) {
    const rx = index === 0x1c12;
    const opts = pdos.filter((p) => p.rx === rx).map((p) => [p.index, `${hex(p.index, 4)} ${p.name}`]);
    if (opts.length) return opts;
  }
  return ENUMS[objKey(index, sub)] ?? (type === 'bool' ? BOOL : null);
}

// JSON-Wert (bool/Zahl) als Zahl für Auswahllisten
export const asNum = (v) => (typeof v === 'boolean' ? Number(v) : v);

export function formatValue(v, type, opts) {
  if (v === null || v === undefined) return '–';
  const n = asNum(v);
  const hit = opts?.find(([o]) => o === n);
  if (hit) return hit[1];
  if (type === 'f32') return Number(n.toPrecision(7)).toLocaleString('de-DE', { maximumFractionDigits: 7 });
  if (type === 'u16' || type === 'u32') return `${n} (${hex(n, type === 'u16' ? 4 : 8)})`;
  if (type === 'u64') return n.toLocaleString('de-DE');
  return String(n);
}

// Freie Eingabe: dezimal (Komma oder Punkt) oder hexadezimal mit 0x
export function parseValue(text, type) {
  const s = String(text).trim().replace(',', '.');
  const n = /^-?0x[0-9a-f]+$/i.test(s) ? parseInt(s, 16) : Number(s);
  if (s === '' || !Number.isFinite(n)) throw new Error(`Ungültiger Wert: ${text}`);
  if (type !== 'f32' && !Number.isInteger(n)) throw new Error(`Ganzzahl erwartet: ${text}`);
  return type === 'bool' ? n !== 0 : n;
}
