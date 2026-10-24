/**
 * @file CoE-Objekte der EL3356: Adressformat, Wertebereiche (Enums) und Eingabe-Parsing.
 */

/** Basis hexadezimaler Zahlen. */
const HEX_RADIX = 16;
/** Hex-Stellen eines Objektindex (UINT16). */
export const INDEX_HEX_DIGITS = 4;
/** Hex-Stellen eines Subindex (USINT). */
export const SUB_HEX_DIGITS = 2;
/** Hex-Stellen eines UDINT. */
const U32_HEX_DIGITS = 8;
/** Signifikante Stellen eines REAL32 (24 Bit Mantisse). */
export const F32_DIGITS = 7;

// --- Objekte (Beckhoff-ESI EL3356-0010, siehe src/el3356.hpp) ---
/** RxPDO assign (SM2). */
export const OBJ_RX_ASSIGN = 0x1c12;
/** TxPDO assign (SM3). */
export const OBJ_TX_ASSIGN = 0x1c13;
/** RMB Outputs (Steuerbits). */
export const OBJ_CONTROL = 0x7000;
/** Subindex 0: Anzahl der Einträge. */
const SUB_COUNT = 0;

// --- Kommandos für 0xFB00:01 ---
const CMD_NONE = 0x0000;
const CMD_ZERO_BALANCE = 0x0101;
const CMD_CALIBRATION = 0x0102;

/**
 * Zahl als Hex-Text mit Präfix 0x.
 * @param {number} v Wert
 * @param {number} digits Mindeststellen
 * @returns {string} z. B. "0x1A00"
 */
export const hex = (v, digits) => '0x' + Number(v).toString(HEX_RADIX).toUpperCase().padStart(digits, '0');
/**
 * Objektadresse "0xIIII:SS".
 * @param {number} index Index
 * @param {number} sub Subindex
 * @returns {string}
 */
export const objKey = (index, sub) =>
  `${hex(index, INDEX_HEX_DIGITS)}:${Number(sub).toString(HEX_RADIX).toUpperCase().padStart(SUB_HEX_DIGITS, '0')}`;

/** 0x8000:11/:12 Mode0/Mode1 filter settings (ESI: DT0800EN16/DT0801EN16). */
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

/** Auswahl für BOOL-Objekte. */
const BOOL = [
  [0, 'FALSE'],
  [1, 'TRUE'],
];

/** Feste Wertebereiche je Objekt; PDO-Zuordnung (0x1C12/0x1C13) ergibt sich aus dem Katalog. */
const ENUMS = {
  '0x7000:04': [
    [0, 'Mode 0'],
    [1, 'Mode 1'],
  ],
  '0x8000:11': FILTER_OPTIONS,
  '0x8000:12': FILTER_OPTIONS,
  '0xFB00:01': [
    [CMD_NONE, `${hex(CMD_NONE, INDEX_HEX_DIGITS)} Kein Kommando`],
    [CMD_ZERO_BALANCE, `${hex(CMD_ZERO_BALANCE, INDEX_HEX_DIGITS)} Nullabgleich`],
    [CMD_CALIBRATION, `${hex(CMD_CALIBRATION, INDEX_HEX_DIGITS)} Kalibrierung mit Reference load`],
  ],
};

/**
 * Auswahlliste für ein Objekt.
 * @param {number} index Index
 * @param {number} sub Subindex
 * @param {string} type Datentyp ("bool", "u16", ...)
 * @param {object[]} [pdos=[]] PDO-Katalog (für 0x1C12/0x1C13)
 * @returns {Array<[number, string]>|null} [Wert, Text]-Paare oder null (freie Eingabe)
 */
export function enumFor(index, sub, type, pdos = []) {
  if ((index === OBJ_RX_ASSIGN || index === OBJ_TX_ASSIGN) && sub > SUB_COUNT) {
    const rx = index === OBJ_RX_ASSIGN;
    const opts = pdos.filter((p) => p.rx === rx).map((p) => [p.index, `${hex(p.index, INDEX_HEX_DIGITS)} ${p.name}`]);
    if (opts.length) return opts;
  }
  return ENUMS[objKey(index, sub)] ?? (type === 'bool' ? BOOL : null);
}

/**
 * JSON-Wert (bool/Zahl) als Zahl für Auswahllisten.
 * @param {boolean|number} v Wert
 * @returns {number}
 */
export const asNum = (v) => (typeof v === 'boolean' ? Number(v) : v);

/**
 * Anzeigetext eines Objektwerts.
 * @param {boolean|number|null|undefined} v Wert
 * @param {string} type Datentyp
 * @param {Array<[number, string]>|null} [opts] Auswahlliste (Text statt Zahl)
 * @returns {string}
 */
export function formatValue(v, type, opts) {
  if (v === null || v === undefined) return '–';
  const n = asNum(v);
  const hit = opts?.find(([o]) => o === n);
  if (hit) return hit[1];
  if (type === 'f32')
    return Number(n.toPrecision(F32_DIGITS)).toLocaleString('de-DE', { maximumFractionDigits: F32_DIGITS });
  if (type === 'u16' || type === 'u32') return `${n} (${hex(n, type === 'u16' ? INDEX_HEX_DIGITS : U32_HEX_DIGITS)})`;
  if (type === 'u64') return n.toLocaleString('de-DE');
  return String(n);
}

/**
 * Wertet eine freie Eingabe aus: dezimal (Komma oder Punkt) oder hexadezimal mit 0x.
 * @param {string} text Eingabe
 * @param {string} type Datentyp; Ganzzahltypen verlangen ganze Zahlen
 * @returns {number|boolean} Wert (bool für "bool")
 * @throws {Error} bei ungültiger Eingabe
 */
export function parseValue(text, type) {
  const s = String(text).trim().replace(',', '.');
  const n = /^-?0x[0-9a-f]+$/i.test(s) ? parseInt(s, HEX_RADIX) : Number(s);
  if (s === '' || !Number.isFinite(n)) throw new Error(`Ungültiger Wert: ${text}`);
  if (type !== 'f32' && !Number.isInteger(n)) throw new Error(`Ganzzahl erwartet: ${text}`);
  return type === 'bool' ? n !== 0 : n;
}
