/**
 * @file CoE objects of the EL3356: address format, selection lists and units from the device description
 * (/api/device/description, generated from the ESI) and input parsing.
 */

/** Base of hexadecimal numbers. */
const HEX_RADIX = 16;
/** Hex digits of an object index (UINT16). */
export const INDEX_HEX_DIGITS = 4;
/** Hex digits of a subindex (USINT). */
export const SUB_HEX_DIGITS = 2;
/** Hex digits of a UDINT. */
const U32_HEX_DIGITS = 8;
/** Significant digits of a REAL32 (24-bit mantissa). */
export const F32_DIGITS = 7;

// --- Objects (Beckhoff ESI EL3356-0010, see src/el3356.hpp) ---
/** RxPDO assign (SM2). */
export const OBJ_RX_ASSIGN = 0x1c12;
/** TxPDO assign (SM3). */
export const OBJ_TX_ASSIGN = 0x1c13;
/** RMB Outputs (control bits). */
export const OBJ_CONTROL = 0x7000;
/** Subindex 0: number of entries. */
const SUB_COUNT = 0;

/** Request object of the commands (0xFB00:01). */
const CMD_REQUEST_KEY = '0xFB00:01';
/** 0xFB00:01 = 0: no command. */
const CMD_NONE = 0x0000;
/** First configuration object (ETG.5001 Modular Device Profile). */
const OBJ_CONFIG_FIRST = 0x8000;
/** Last configuration object (ETG.5001 Modular Device Profile). */
const OBJ_CONFIG_LAST = 0x8fff;
/** Decimal exponents with SI prefix for units of the ESI. */
const SI_PREFIX = { '-3': 'm', '-6': 'µ', 3: 'k' };
/** Powers of ten as base. */
const DECIMAL_BASE = 10;

/**
 * Number as hex text with prefix 0x.
 * @param {number} v value
 * @param {number} digits minimum digits
 * @returns {string} e.g. "0x1A00"
 */
export const hex = (v, digits) => '0x' + Number(v).toString(HEX_RADIX).toUpperCase().padStart(digits, '0');
/**
 * Object address "0xIIII:SS".
 * @param {number} index index
 * @param {number} sub subindex
 * @returns {string}
 */
export const objKey = (index, sub) =>
  `${hex(index, INDEX_HEX_DIGITS)}:${Number(sub).toString(HEX_RADIX).toUpperCase().padStart(SUB_HEX_DIGITS, '0')}`;

/** Selection for BOOL objects. */
const BOOL = [
  [0, 'FALSE'],
  [1, 'TRUE'],
];

/**
 * Entry of the device description.
 * @param {object|null} desc device description (/api/device/description)
 * @param {string} key address "0xIIII:SS"
 * @returns {object|null} entry with index (number) or null
 */
export function descEntry(desc, key) {
  if (!desc) return null;
  const [idx, sub] = key.split(':');
  const obj = desc.objects.find((o) => o.index === idx);
  const e = obj?.entries.find((x) => x.sub === `0x${sub}`);
  return e ? { ...e, index: parseInt(idx, HEX_RADIX), subNum: parseInt(sub, HEX_RADIX), key } : null;
}

/**
 * Hex text of the description ("0200", little endian) as a number.
 * @param {string} data bytes as hex text
 * @returns {number}
 */
function leHexValue(data) {
  let v = 0;
  for (let i = data.length - SUB_HEX_DIGITS; i >= 0; i -= SUB_HEX_DIGITS) v = v * (HEX_RADIX * HEX_RADIX) + parseInt(data.substr(i, SUB_HEX_DIGITS), HEX_RADIX);
  return v;
}

/**
 * Selection list of an entry according to the device description (enumeration or values of the supplement file).
 * @param {object|null} desc device description
 * @param {object|null} e entry
 * @returns {Array<[number, string]>|null}
 */
export function entryOptions(desc, e) {
  if (!desc || !e) return null;
  if (e.enum) return desc.enums[e.enum].map((x) => [x.value, x.text]);
  if (e.values) return e.values.map((x) => [x.value, x.text]);
  return null;
}

/**
 * Selection list for the command object 0xFB00:01 from the commands of the device description.
 * @param {object} desc device description
 * @returns {Array<[number, string]>}
 */
function commandOptions(desc) {
  const opts = [[CMD_NONE, `${hex(CMD_NONE, INDEX_HEX_DIGITS)} Kein Kommando`]];
  for (const c of desc.commands) {
    const w = c.writes.find((x) => x.obj === CMD_REQUEST_KEY);
    if (!w) continue;
    const v = leHexValue(w.data);
    if (!opts.some(([o]) => o === v)) opts.push([v, `${hex(v, INDEX_HEX_DIGITS)} ${c.label_de ?? c.id}`]);
  }
  return opts;
}

/**
 * Selection list for an object.
 * @param {number} index index
 * @param {number} sub subindex
 * @param {string} type data type ("bool", "u16", ...)
 * @param {object[]} [pdos=[]] PDO catalog (for 0x1C12/0x1C13)
 * @param {object|null} [desc=null] device description (enumerations, commands)
 * @returns {Array<[number, string]>|null} [value, text] pairs or null (free input)
 */
export function enumFor(index, sub, type, pdos = [], desc = null) {
  if ((index === OBJ_RX_ASSIGN || index === OBJ_TX_ASSIGN) && sub > SUB_COUNT) {
    const rx = index === OBJ_RX_ASSIGN;
    const opts = pdos.filter((p) => p.rx === rx).map((p) => [p.index, `${hex(p.index, INDEX_HEX_DIGITS)} ${p.name}`]);
    if (opts.length) return opts;
  }
  const key = objKey(index, sub);
  if (desc && key === CMD_REQUEST_KEY) return commandOptions(desc);
  return entryOptions(desc, descEntry(desc, key)) ?? (type === 'bool' ? BOOL : null);
}

/**
 * Writable terminal parameters according to the device description (like el3356::device_settings in the backend).
 * @param {object|null} desc device description
 * @returns {object[]} entries (see descEntry) by index and subindex
 */
export function settingEntries(desc) {
  if (!desc) return [];
  const out = [];
  for (const o of desc.objects) {
    const idx = parseInt(o.index, HEX_RADIX);
    if (idx < OBJ_CONFIG_FIRST || idx > OBJ_CONFIG_LAST) continue;
    for (const e of o.entries) {
      const sub = parseInt(e.sub, HEX_RADIX);
      if (sub === SUB_COUNT || e.access !== 'rw' || e.never_write || ['string', 'bytes'].includes(e.type)) continue;
      out.push(descEntry(desc, objKey(idx, sub)));
    }
  }
  return out;
}

/**
 * Display unit of an entry: unit of the TwinCAT dialog, otherwise unit of the ESI.
 * @param {object} e entry
 * @returns {string} e.g. "s", "mV/V"; empty without a unit
 */
export function entryUnit(e) {
  if (e.convert?.unit) return e.convert.unit;
  if (!e.unit?.unit) return '';
  const prefix = SI_PREFIX[e.unit.exp];
  return prefix ? prefix + e.unit.unit : e.unit.unit;
}

/**
 * Conversion factor raw value -> display: ConvertValue of the TwinCAT dialog, otherwise the decimal exponent
 * of the ESI unit if it has no SI prefix (e.g. 0x7000:11 in 0.1 Hz -> display in Hz).
 * @param {object} e entry
 * @returns {number} factor, 1 without conversion
 */
export function entryFactor(e) {
  if (e.convert) return e.convert.factor ?? 1;
  const exp = e.unit?.exp ?? 0;
  return exp && !SI_PREFIX[exp] ? DECIMAL_BASE ** exp : 1;
}

/**
 * JSON value (bool/number) as a number for selection lists.
 * @param {boolean|number} v value
 * @returns {number}
 */
export const asNum = (v) => (typeof v === 'boolean' ? Number(v) : v);

/**
 * Display text of an object value.
 * @param {boolean|number|null|undefined} v value
 * @param {string} type data type
 * @param {Array<[number, string]>|null} [opts] selection list (text instead of number)
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
 * Evaluates a free input: decimal (comma or point) or hexadecimal with 0x.
 * @param {string} text input
 * @param {string} type data type; integer types require whole numbers
 * @returns {number|boolean} value (bool for "bool")
 * @throws {Error} for invalid input
 */
export function parseValue(text, type) {
  const s = String(text).trim().replace(',', '.');
  const n = /^-?0x[0-9a-f]+$/i.test(s) ? parseInt(s, HEX_RADIX) : Number(s);
  if (s === '' || !Number.isFinite(n)) throw new Error(`Ungültiger Wert: ${text}`);
  if (type !== 'f32' && !Number.isInteger(n)) throw new Error(`Ganzzahl erwartet: ${text}`);
  return type === 'bool' ? n !== 0 : n;
}
