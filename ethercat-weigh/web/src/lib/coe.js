/**
 * @file CoE-Objekte der EL3356: Adressformat, Auswahllisten und Einheiten aus der Gerätebeschreibung
 * (/api/device/description, erzeugt aus der ESI) und Eingabe-Parsing.
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

/** Request-Objekt der Kommandos (0xFB00:01). */
const CMD_REQUEST_KEY = '0xFB00:01';
/** 0xFB00:01 = 0: kein Kommando. */
const CMD_NONE = 0x0000;
/** Erstes Konfigurationsobjekt (ETG.5001 Modular Device Profile). */
const OBJ_CONFIG_FIRST = 0x8000;
/** Letztes Konfigurationsobjekt (ETG.5001 Modular Device Profile). */
const OBJ_CONFIG_LAST = 0x8fff;
/** Zehnerexponenten mit SI-Vorsatz für Einheiten der ESI. */
const SI_PREFIX = { '-3': 'm', '-6': 'µ', 3: 'k' };
/** Zehnerpotenzen als Basis. */
const DECIMAL_BASE = 10;

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

/** Auswahl für BOOL-Objekte. */
const BOOL = [
  [0, 'FALSE'],
  [1, 'TRUE'],
];

/**
 * Eintrag der Gerätebeschreibung.
 * @param {object|null} desc Gerätebeschreibung (/api/device/description)
 * @param {string} key Adresse "0xIIII:SS"
 * @returns {object|null} Eintrag mit index (Zahl) oder null
 */
export function descEntry(desc, key) {
  if (!desc) return null;
  const [idx, sub] = key.split(':');
  const obj = desc.objects.find((o) => o.index === idx);
  const e = obj?.entries.find((x) => x.sub === `0x${sub}`);
  return e ? { ...e, index: parseInt(idx, HEX_RADIX), subNum: parseInt(sub, HEX_RADIX), key } : null;
}

/**
 * Hex-Text der Beschreibung ("0200", little endian) als Zahl.
 * @param {string} data Bytes als Hex-Text
 * @returns {number}
 */
function leHexValue(data) {
  let v = 0;
  for (let i = data.length - SUB_HEX_DIGITS; i >= 0; i -= SUB_HEX_DIGITS) v = v * (HEX_RADIX * HEX_RADIX) + parseInt(data.substr(i, SUB_HEX_DIGITS), HEX_RADIX);
  return v;
}

/**
 * Auswahlliste eines Eintrags laut Gerätebeschreibung (Aufzählung oder Werte der Zusatzdatei).
 * @param {object|null} desc Gerätebeschreibung
 * @param {object|null} e Eintrag
 * @returns {Array<[number, string]>|null}
 */
export function entryOptions(desc, e) {
  if (!desc || !e) return null;
  if (e.enum) return desc.enums[e.enum].map((x) => [x.value, x.text]);
  if (e.values) return e.values.map((x) => [x.value, x.text]);
  return null;
}

/**
 * Auswahlliste für das Kommando-Objekt 0xFB00:01 aus den Kommandos der Gerätebeschreibung.
 * @param {object} desc Gerätebeschreibung
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
 * Auswahlliste für ein Objekt.
 * @param {number} index Index
 * @param {number} sub Subindex
 * @param {string} type Datentyp ("bool", "u16", ...)
 * @param {object[]} [pdos=[]] PDO-Katalog (für 0x1C12/0x1C13)
 * @param {object|null} [desc=null] Gerätebeschreibung (Aufzählungen, Kommandos)
 * @returns {Array<[number, string]>|null} [Wert, Text]-Paare oder null (freie Eingabe)
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
 * Schreibbare Klemmenparameter laut Gerätebeschreibung (wie el3356::device_settings im Backend).
 * @param {object|null} desc Gerätebeschreibung
 * @returns {object[]} Einträge (siehe descEntry) nach Index und Subindex
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
 * Anzeigeeinheit eines Eintrags: Einheit des TwinCAT-Dialogs, sonst Einheit der ESI.
 * @param {object} e Eintrag
 * @returns {string} z. B. "s", "mV/V"; leer ohne Einheit
 */
export function entryUnit(e) {
  if (e.convert?.unit) return e.convert.unit;
  if (!e.unit?.unit) return '';
  const prefix = SI_PREFIX[e.unit.exp];
  return prefix ? prefix + e.unit.unit : e.unit.unit;
}

/**
 * Umrechnungsfaktor Rohwert -> Anzeige: ConvertValue des TwinCAT-Dialogs, sonst der Zehnerexponent
 * der ESI-Einheit, wenn er keinen SI-Vorsatz hat (z. B. 0x7000:11 in 0,1 Hz -> Anzeige in Hz).
 * @param {object} e Eintrag
 * @returns {number} Faktor, 1 ohne Umrechnung
 */
export function entryFactor(e) {
  if (e.convert) return e.convert.factor ?? 1;
  const exp = e.unit?.exp ?? 0;
  return exp && !SI_PREFIX[exp] ? DECIMAL_BASE ** exp : 1;
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
