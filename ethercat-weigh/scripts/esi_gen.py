#!/usr/bin/env python3
"""Erzeugt die Gerätebeschreibung (JSON) eines EtherCAT-Slaves aus einer Beckhoff-ESI.

Gelesen werden alle Revisionen des Gerätetyps; Basis ist die neueste. Was nicht in jeder
Revision vorkommt, trägt die Liste "revisions" der Revisionen, in denen es vorkommt.

Inhalt der Ausgabe:
- device: Typ, Produktcode, Revisionen, Namen, Quelle
- enums: Aufzählungstypen (DTxxxxENnn)
- objects: Objektverzeichnis mit Einträgen (Typ, Bitlage, Zugriff, Default, Grenzen, Einheit)
  und den Angaben des TwinCAT-SettingsTab (Labels, Tooltips, Skalierung, Abhängigkeiten)
- pdos: RxPDOs/TxPDOs mit Exclude-Listen und Mapping je Revision
- pdo_rules: bedingte PDO-Zuordnung laut SettingsTab
- variants: AlternativeSmMapping (PDO-Zuordnung, DC-Mode, Init-Kommandos)
- dc_modes: Dc/OpMode
- commands: Kommandoknöpfe des SettingsTab und Kommandos der Zusatzdatei
- warnings: nicht auflösbare Verweise

Die Zusatzdatei (--supplement) ergänzt Angaben, die die ESI nicht enthält. Jede Angabe trägt
eine Quelle; übernommene Felder stehen im Eintrag unter "supplement".

Die Ausgabe ist stabil sortiert. --check vergleicht mit der vorhandenen Datei, ohne zu schreiben.

Beispiel:
    esi_gen.py "esi/Beckhoff EL3356-0010.xml" --type EL3356-0010 \\
        --supplement scripts/esi_supplement.el3356_0010.json -o src/generated/el3356_0010.json
"""

import argparse
import json
import math
import struct
import sys
import xml.etree.ElementTree as ET
from pathlib import Path

from esi_common import decode

# LcId der Texte in der ESI
LCID_DE = '1031'
LCID_EN = '1033'

# ESI-Basistyp -> Kurzname (wie dtype_name() in src/el3356.cpp)
BASE_TYPES = {
    'BOOL': 'bool', 'BYTE': 'u8', 'USINT': 'u8', 'UINT': 'u16', 'WORD': 'u16',
    'UDINT': 'u32', 'DWORD': 'u32', 'ULINT': 'u64', 'SINT': 'i8', 'INT': 'i16',
    'DINT': 'i32', 'REAL': 'f32',
}
SIGNED = {'i8', 'i16', 'i32'}

# Einheitencodes nach ETG.1004 (Unit Specification): Byte 3 = Zehnerexponent (vorzeichenbehaftet),
# Byte 2 = Zählereinheit, Byte 1 = Nennereinheit. Nur die in den verwendeten ESI-Dateien
# vorkommenden Einheiten; unbekannte Codes führen zum Abbruch.
UNITS = {0x00: '', 0x03: 's', 0x20: 'Hz', 0x26: 'V', 0x55: 'm/s²'}
UNIT_BYTE_MASK = 0xFF
UNIT_EXP_SHIFT = 24
UNIT_NUM_SHIFT = 16
UNIT_DEN_SHIFT = 8
SIGN_BIT_8 = 0x80

# SettingsTab: Bit 15 einer SubMode- bzw. OpMode-Referenz; die Elementnummer steht in Bit 0..14.
# Die Bedeutung von Bit 15 ist nicht dokumentiert; beide Formen werden gleich behandelt.
REF_MASK = 0x7FFF
# Dialogelemente, deren einziges InitCmd den bearbeiteten Eintrag bezeichnet
FIELD_ELEMENTS = {'EditField', 'CheckBoxChecked', 'ComboBox', 'OnlineField', 'OutputField'}
COMMAND_ELEMENTS = {'CmdObjectButton'}


class EsiError(Exception):
    """Unerwarteter Inhalt der ESI oder der Zusatzdatei."""


def num(text: str | None) -> int | None:
    """Zahl in ESI-Schreibweise ("#x1F" oder dezimal).

    @param text Text oder None
    @return Wert oder None
    """
    if text is None:
        return None
    t = text.strip()
    return int(t[2:], 16) if t.lower().startswith('#x') else int(t)


def hex16(v: int) -> str:
    """Index als "0xIIII"."""
    return f'0x{v:04X}'


def hex8(v: int) -> str:
    """Subindex als "0xSS"."""
    return f'0x{v:02X}'


def addr(index: int, sub: int) -> str:
    """Adresse als "0xIIII:SS"."""
    return f'{hex16(index)}:{sub:02X}'


def rev_str(rev: int) -> str:
    """Revision als "0xRRRRRRRR"."""
    return f'0x{rev:08X}'


def text(el: ET.Element, tag: str, lcid: str | None = None) -> str | None:
    """Text eines Kindelements, bei lcid das mit passendem LcId; Leerraum zusammengefasst.

    @return Text oder None
    """
    for c in el.findall(tag):
        if lcid is None or c.get('LcId') == lcid:
            return ' '.join((c.text or '').split())
    return None


def f32_repr(v: float) -> float:
    """Kürzeste Dezimaldarstellung, die als REAL32 denselben Wert ergibt."""
    if not math.isfinite(v):
        raise EsiError(f'REAL-Wert {v} nicht darstellbar')
    bits = struct.pack('<f', v)
    for digits in range(1, 10):
        cand = float(f'{v:.{digits}g}')
        if struct.pack('<f', cand) == bits:
            return cand
    return v


def decode_le(data: bytes, dtype: str):
    """Wert aus Bytes (little endian) gemäß Kurzname.

    @return bool, int, float, str (string) oder Hex-Text (bytes)
    """
    if dtype == 'f32':
        return f32_repr(struct.unpack('<f', data[:4])[0])
    if dtype == 'bool':
        return bool(data[0] & 1)
    if dtype == 'string':
        return data.split(b'\0', 1)[0].decode('latin-1')
    if dtype == 'bytes':
        return data.hex().upper()
    return int.from_bytes(data, 'little', signed=dtype in SIGNED)


def to_signed(v: int, bits: int) -> int:
    """Zweierkomplement für "#x"-Werte vorzeichenbehafteter Typen."""
    return v - (1 << bits) if v >= 1 << (bits - 1) else v


def unit_info(code: int) -> dict:
    """Zerlegt einen Einheitencode (ETG.1004).

    @return {"code", "exp", "unit"}; unit z. B. "mV/V" ohne Exponent als "V/V" mit exp
    """
    exp = (code >> UNIT_EXP_SHIFT) & UNIT_BYTE_MASK
    if exp & SIGN_BIT_8:
        exp -= 1 << 8
    n = (code >> UNIT_NUM_SHIFT) & UNIT_BYTE_MASK
    d = (code >> UNIT_DEN_SHIFT) & UNIT_BYTE_MASK
    if n not in UNITS or d not in UNITS:
        raise EsiError(f'Einheitencode 0x{code:08X} unbekannt')
    unit = UNITS[n] + (f'/{UNITS[d]}' if d else '')
    return {'code': f'0x{code:08X}', 'exp': exp, 'unit': unit}


# --- Datentypen und Objekte ----------------------------------------------------------------


class Types:
    """DataTypes eines Dictionary."""

    def __init__(self, dic: ET.Element):
        self.by_name = {t.findtext('Name'): t for t in dic.find('DataTypes')}

    def resolve(self, name: str) -> tuple[str, str | None]:
        """Kurzname und Enum-Name eines Typs.

        @return (Kurzname, Enum-Name oder None); "string"/"bytes" für Zeichenketten/Byte-Arrays
        """
        if name in BASE_TYPES:
            return BASE_TYPES[name], None
        if name.startswith('STRING('):
            return 'string', None
        t = self.by_name.get(name)
        if t is None:
            raise EsiError(f'Datentyp {name} unbekannt')
        if t.find('EnumInfo') is not None:
            return BASE_TYPES[t.findtext('BaseType')], name
        if t.find('ArrayInfo') is not None and t.findtext('BaseType') == 'BYTE':
            return 'bytes', None
        if t.findtext('BaseType') in BASE_TYPES:
            return BASE_TYPES[t.findtext('BaseType')], None
        raise EsiError(f'Datentyp {name} nicht unterstützt')

    def enums(self) -> dict:
        """Alle Aufzählungstypen: Name -> [{"value", "text"}]."""
        out = {}
        for name, t in self.by_name.items():
            items = t.findall('EnumInfo')
            if items:
                out[name] = [{'value': num(e.findtext('Enum')), 'text': text(e, 'Text')} for e in items]
        return out


def flags(el: ET.Element | None) -> dict:
    """Access, WriteRestrictions, PdoMapping, Setting, Backup aus Flags."""
    out = {}
    if el is None:
        return out
    acc = el.find('Access')
    if acc is not None:
        out['access'] = acc.text.strip()
        if acc.get('WriteRestrictions'):
            out['write_restrictions'] = acc.get('WriteRestrictions')
    if el.findtext('PdoMapping'):
        out['pdo_mapping'] = el.findtext('PdoMapping').strip()
    for tag in ('Setting', 'Backup'):
        if num(el.findtext(tag)):
            out[tag.lower()] = True
    return out


def apply_info(entry: dict, info: ET.Element | None):
    """DefaultData/DefaultValue, MinValue, MaxValue, Unit eines Eintrags."""
    if info is None:
        return
    dtype, bits = entry['type'], entry['bits']

    def scalar(v: int):
        if dtype == 'f32':
            return f32_repr(struct.unpack('<f', v.to_bytes(4, 'little'))[0])
        if dtype == 'bool':
            return bool(v)
        return to_signed(v, bits) if dtype in SIGNED else v

    if info.findtext('DefaultData'):
        entry['default'] = decode_le(bytes.fromhex(info.findtext('DefaultData').strip()), dtype)
    elif info.findtext('DefaultValue'):
        entry['default'] = scalar(num(info.findtext('DefaultValue')))
    if info.findtext('MinValue'):
        entry['min'] = scalar(num(info.findtext('MinValue')))
    if info.findtext('MaxValue'):
        entry['max'] = scalar(num(info.findtext('MaxValue')))
    if info.findtext('Unit'):
        entry['unit'] = unit_info(num(info.findtext('Unit')))


def object_entries(obj: ET.Element, types: Types) -> list[dict]:
    """Einträge eines Objekts; Arrays werden in Elemente aufgelöst.

    @return Einträge mit sub, name, type, bits, bit_offset und Flags
    """
    tname = obj.findtext('Type')
    t = types.by_name.get(tname)
    obj_flags = flags(obj.find('Flags'))
    if t is None or t.find('SubItem') is None:
        dtype, enum = types.resolve(tname)
        e = {'sub': 0, 'name': obj.findtext('Name'), 'type': dtype, 'esi_type': tname,
             'bits': num(obj.findtext('BitSize')), **obj_flags}
        if enum:
            e['enum'] = enum
        apply_info(e, obj.find('Info'))
        return [e]

    infos = {s.findtext('Name'): s.find('Info') for s in obj.findall('Info/SubItem')}
    out = []
    for si in t.findall('SubItem'):
        stype = si.findtext('Type')
        st = types.by_name.get(stype)
        base = {**obj_flags, **flags(si.find('Flags'))}
        if st is not None and st.find('ArrayInfo') is not None and si.findtext('SubIdx') is None:
            # Array-Subitem (z. B. 0x1C12 "Elements"): ein Eintrag je Element ab LBound
            lb = num(st.findtext('ArrayInfo/LBound'))
            n = num(st.findtext('ArrayInfo/Elements'))
            ebits = num(si.findtext('BitSize')) // n
            dtype, enum = types.resolve(st.findtext('BaseType'))
            for k in range(n):
                sub = lb + k
                name = f'SubIndex {sub:03d}'
                e = {'sub': sub, 'name': name, 'type': dtype, 'esi_type': st.findtext('BaseType'),
                     'bits': ebits, 'bit_offset': num(si.findtext('BitOffs')) + k * ebits, **base}
                apply_info(e, infos.get(name))
                out.append(e)
            continue
        dtype, enum = types.resolve(stype)
        e = {'sub': num(si.findtext('SubIdx')), 'name': si.findtext('Name'), 'type': dtype,
             'esi_type': stype, 'bits': num(si.findtext('BitSize')),
             'bit_offset': num(si.findtext('BitOffs')), **base}
        if enum:
            e['enum'] = enum
        apply_info(e, infos.get(e['name']))
        out.append(e)
    return out


def device_objects(dev: ET.Element) -> tuple[dict, dict]:
    """Objekte und Enums einer Revision.

    @return ({index: {"name", "type", "access", "entries": {sub: entry}}}, enums)
    """
    dic = dev.find('Profile/Dictionary')
    types = Types(dic)
    objs = {}
    for o in dic.find('Objects'):
        idx = num(o.findtext('Index'))
        objs[idx] = {
            'name': o.findtext('Name'), 'type': o.findtext('Type'),
            **{k: v for k, v in flags(o.find('Flags')).items() if k in ('access', 'write_restrictions')},
            'entries': {e['sub']: e for e in object_entries(o, types)},
        }
    return objs, types.enums()


# --- PDOs, Varianten, DC --------------------------------------------------------------------


def device_pdos(dev: ET.Element) -> dict:
    """PDOs einer Revision: {index: pdo}."""
    out = {}
    for tag, rx in (('RxPdo', True), ('TxPdo', False)):
        for p in dev.findall(tag):
            idx = num(p.findtext('Index'))
            entries = []
            for e in p.findall('Entry'):
                ei = num(e.findtext('Index'))
                pe = {'index': hex16(ei), 'bits': num(e.findtext('BitLen'))}
                if ei:
                    pe.update({'sub': hex8(num(e.findtext('SubIndex'))), 'name': e.findtext('Name'),
                               'type': BASE_TYPES.get(e.findtext('DataType'), e.findtext('DataType'))})
                entries.append(pe)
            pdo = {'index': hex16(idx), 'name': p.findtext('Name'), 'dir': 'rx' if rx else 'tx',
                   'fixed': bool(num(p.get('Fixed', '0'))),
                   'exclude': sorted(hex16(num(x.text)) for x in p.findall('Exclude')),
                   'entries': entries}
            if p.get('Sm') is not None:
                pdo['sm'] = num(p.get('Sm'))
            out[idx] = pdo
    return out


def device_variants(dev: ET.Element) -> list[dict]:
    """AlternativeSmMapping einer Revision in ESI-Reihenfolge."""
    out = []
    for m in dev.findall('Info/VendorSpecific/TwinCAT/AlternativeSmMapping'):
        v = {'name': text(m, 'Name'), 'default': bool(num(m.get('Default', '0')))}
        for sm in m.findall('Sm'):
            no = num(sm.get('No'))
            v[f'sm{no}'] = [hex16(num(p.text)) for p in sm.findall('Pdo')]
            excl = [hex16(num(p.text)) for p in sm.findall('ExcludedPdo')]
            if excl:
                v[f'sm{no}_excluded'] = excl
        op = m.find('OperationMode')
        if op is not None:
            v['dc_mode'] = op.findtext('DcOpModeName')
            v['init'] = [{
                'index': hex16(num(c.findtext('Index'))), 'sub': hex8(num(c.findtext('SubIndex'))),
                'transition': [t.text for t in c.findall('Transition')],
                'complete_access': bool(num(c.get('CompleteAccess', '0'))),
                'data': ''.join(c.findtext('Data').split()).upper(),
            } for c in op.findall('InitCmds')]
        out.append(v)
    return out


def device_dc_modes(dev: ET.Element) -> list[dict]:
    """Dc/OpMode einer Revision."""
    out = []
    for op in dev.findall('Dc/OpMode'):
        m = {'name': op.findtext('Name'), 'desc': op.findtext('Desc'),
             'assign_activate': hex16(num(op.findtext('AssignActivate')))}
        for tag in ('CycleTimeSync0', 'ShiftTimeSync0', 'CycleTimeSync1', 'ShiftTimeSync1'):
            el = op.find(tag)
            if el is None:
                continue
            d = {'value': num(el.text)}
            for a in ('Factor', 'Input'):
                if el.get(a) is not None:
                    d[a.lower()] = num(el.get(a))
            m[tag[0].lower() + tag[1:]] = d
        out.append(m)
    return out


# --- SettingsTab -----------------------------------------------------------------------------


def settings_tab(dev: ET.Element, objs: dict, enums: dict, variants: list[dict],
                 warnings: list[str]) -> tuple[dict, list[dict], list[dict]]:
    """Labels, Tooltips, Skalierung und Abhängigkeiten aus dem TwinCAT-SettingsTab.

    Abhängigkeiten (Enable): Ein Feld ist aktiv, wenn mindestens eine Bedingung erfüllt ist.
    - SubMode ohne Index: das referenzierte Kontrollkästchen ist gesetzt (eq true).
    - SubMode mit Index: die referenzierte Auswahl steht auf Eintrag Index (ab 1 gezählt) der
      Aufzählung.
    - OpMode: die Variante (AlternativeSmMapping, ab 1 gezählt) ist gewählt.

    @return ({(index, sub): Zusatzfelder}, pdo_rules, commands)
    """
    tab = dev.find('Info/VendorSpecific/TwinCAT/SettingsTab')
    if tab is None:
        return {}, [], []
    elements = tab.findall('DialogElement')
    by_submode = {num(e.findtext('SubMode')): e for e in elements if e.findtext('SubMode')}

    def target(e: ET.Element) -> tuple[int, int] | None:
        cmds = e.findall('InitCmd')
        if len(cmds) != 1:
            return None
        return num(cmds[0].findtext('Index')), num(cmds[0].findtext('SubIndex'))

    def labels(e: ET.Element) -> dict:
        out = {}
        for key, tag, lcid in (('label_de', 'Name', LCID_DE), ('label_en', 'Name', LCID_EN),
                               ('tooltip_de', 'ToolTipText', LCID_DE),
                               ('tooltip_en', 'ToolTipText', LCID_EN)):
            v = text(e, tag, lcid)
            if v:
                out[key] = v
        return out

    def condition(en: ET.Element, owner: str) -> dict | None:
        if en.findtext('OpMode'):
            no = num(en.findtext('OpMode')) & REF_MASK
            if not 1 <= no <= len(variants):
                warnings.append(f'{owner}: Enable OpMode {no} ohne Variante')
                return None
            return {'variant': variants[no - 1]['name']}
        ref = num(en.findtext('SubMode')) & REF_MASK
        src = by_submode.get(ref)
        tgt = target(src) if src is not None else None
        if tgt is None:
            warnings.append(f'{owner}: Enable SubMode {ref} verweist auf kein Feld')
            return None
        obj = addr(*tgt)
        if en.findtext('Index') is None:
            return {'obj': obj, 'eq': True}
        pos = num(en.findtext('Index'))
        enum = objs[tgt[0]]['entries'][tgt[1]].get('enum')
        if enum is None or not 1 <= pos <= len(enums[enum]):
            warnings.append(f'{owner}: Enable Index {pos} für {obj} nicht auflösbar')
            return None
        return {'obj': obj, 'eq': enums[enum][pos - 1]['value']}

    fields, rules, commands = {}, {}, []
    for e in elements:
        kind = e.findtext('DialogElement')
        if kind in COMMAND_ELEMENTS:
            commands.append({
                'id': f'esi_{num(e.findtext("SubMode"))}', 'origin': 'esi', **labels(e),
                'writes': [{'obj': addr(num(c.findtext('Index')), num(c.findtext('SubIndex'))),
                            'data': c.findtext('Data').strip().upper()} for c in e.findall('InitCmd')],
            })
            continue
        if kind not in FIELD_ELEMENTS:
            continue
        tgt = target(e)
        if tgt is None or tgt[0] not in objs or tgt[1] not in objs[tgt[0]]['entries']:
            warnings.append(f'{kind} {num(e.findtext("SubMode"))}: kein eindeutiges Ziel')
            continue
        owner = addr(*tgt)
        f = {'dialog': kind, **labels(e)}
        conds = [c for c in (condition(en, owner) for en in e.findall('Enable')) if c]
        if conds:
            f['enable_if'] = conds
        cv = e.find('ConvertValue')
        if cv is not None:
            a, b = num(cv.findtext('Const_A') or '1'), num(cv.findtext('Const_B') or '0')
            c, d = num(cv.findtext('Const_C') or '1'), num(cv.findtext('Const_D') or '0')
            if b or d:
                raise EsiError(f'{owner}: ConvertValue mit Const_B/Const_D wird nicht unterstützt')
            conv = {}
            if a != 1 or c != 1:
                conv['factor'] = a / c
            if cv.findtext('Unit'):
                conv['unit'] = cv.findtext('Unit').replace('^2', '²')
            if cv.findtext('FloatFormat'):
                conv['decimals'] = num(cv.findtext('FloatFormat'))
            f['convert'] = conv
        if owner in {addr(*k) for k in fields}:
            raise EsiError(f'{owner}: mehrere Dialogfelder')
        fields[tgt] = f
        for p in e.findall('Pdo'):
            key = hex16(num(p.text))
            rules.setdefault(key, {'pdo': key, 'if_any': []})['if_any'].append(
                {'obj': owner, 'eq': num(p.get('Assigned'))})
    return fields, list(rules.values()), commands


# --- Zusammenführen über Revisionen --------------------------------------------------------


def presence(items: list[tuple[int, object]], all_revs: list[int]) -> list[str] | None:
    """Revisionen, in denen ein Element vorkommt; None, wenn in allen."""
    revs = sorted({r for r, _ in items})
    return None if revs == all_revs else [rev_str(r) for r in revs]


def merge_by_key(per_rev: list[tuple[int, dict]], all_revs: list[int]) -> dict:
    """Führt {key: value} je Revision zusammen; der Wert der neuesten Revision gilt.

    @return {key: (value, revisions oder None)}
    """
    seen: dict = {}
    for rev, items in per_rev:
        for k, v in items.items():
            seen.setdefault(k, []).append((rev, v))
    return {k: (max(lst, key=lambda x: x[0])[1], presence(lst, all_revs)) for k, lst in seen.items()}


def build(devs: list[ET.Element], source: str, file_version: str | None, supplement: dict | None,
          supplement_name: str | None) -> dict:
    """Gerätebeschreibung aus allen Revisionen eines Typs."""
    devs = sorted(devs, key=lambda d: num(d.find('Type').get('RevisionNo')))
    revs = [num(d.find('Type').get('RevisionNo')) for d in devs]
    newest = devs[-1]
    warnings: list[str] = []

    per_rev_objs = []
    enums: dict = {}
    for r, d in zip(revs, devs):
        objs, en = device_objects(d)
        per_rev_objs.append((r, objs))
        enums.update(en)  # neueste Revision gilt

    objects = []
    for idx, (obj, obj_revs) in sorted(merge_by_key(
            [(r, o) for r, o in per_rev_objs], revs).items()):
        entries = merge_by_key([(r, o[idx]['entries']) for r, o in per_rev_objs if idx in o], revs)
        out_entries = []
        obj_rev_list = [r for r, o in per_rev_objs if idx in o]
        for sub, (e, e_revs) in sorted(entries.items()):
            e = dict(e)
            sub_revs = sorted(r for r, o in per_rev_objs if idx in o and sub in o[idx]['entries'])
            if sub_revs != obj_rev_list:
                e['revisions'] = [rev_str(r) for r in sub_revs]
            e['sub'] = hex8(sub)
            out_entries.append(e)
        o = {'index': hex16(idx), 'name': obj['name'], 'type': obj['type']}
        for k in ('access', 'write_restrictions'):
            if k in obj:
                o[k] = obj[k]
        if obj_revs:
            o['revisions'] = obj_revs
        o['entries'] = out_entries
        objects.append(o)
    obj_by_index = {int(o['index'], 16): o for o in objects}

    # PDOs: Mapping je Revision gruppieren
    pdos = []
    per_rev_pdos = [(r, device_pdos(d)) for r, d in zip(revs, devs)]
    for idx in sorted({i for _, p in per_rev_pdos for i in p}):
        groups: dict = {}
        latest = None
        for r, p in per_rev_pdos:
            if idx in p:
                key = json.dumps(p[idx]['entries'], sort_keys=True)
                groups.setdefault(key, []).append(r)
                latest = p[idx]
        pdo = {k: v for k, v in latest.items() if k != 'entries'}
        present = sorted(r for g in groups.values() for r in g)
        if present != revs:
            pdo['revisions'] = [rev_str(r) for r in present]
        pdo['layouts'] = [{'revisions': [rev_str(r) for r in g] if len(groups) > 1 else None,
                           'entries': json.loads(k)} for k, g in sorted(groups.items(), key=lambda x: x[1][0])]
        for lay in pdo['layouts']:
            if lay['revisions'] is None:
                del lay['revisions']
        pdos.append(pdo)

    # Varianten: nach Name über Revisionen
    var_items = [(r, {v['name']: dict(v, order=i) for i, v in enumerate(device_variants(d))})
                 for r, d in zip(revs, devs)]
    variants = []
    for name, (v, v_revs) in sorted(merge_by_key(var_items, revs).items(), key=lambda x: x[1][0]['order']):
        v = dict(v)
        del v['order']
        if v_revs:
            v['revisions'] = v_revs
        variants.append(v)

    newest_objs, newest_enums = device_objects(newest)
    newest_variants = device_variants(newest)
    fields, pdo_rules, commands = settings_tab(newest, newest_objs, newest_enums, newest_variants, warnings)
    for (idx, sub), f in fields.items():
        for e in obj_by_index[idx]['entries']:
            if e['sub'] == hex8(sub):
                e.update(f)

    type_el = newest.find('Type')
    desc = {
        'generator': 'scripts/esi_gen.py',
        'source': {'file': source, 'file_version': file_version, 'supplement': supplement_name},
        'device': {
            'type': type_el.text.strip(),
            'product_code': f'0x{num(type_el.get("ProductCode")):08X}',
            'revision': rev_str(revs[-1]),
            'revisions': [rev_str(r) for r in revs],
            'name_de': text(newest, 'Name', LCID_DE),
            'name_en': text(newest, 'Name', LCID_EN),
        },
        'enums': {k: enums[k] for k in sorted(enums)},
        'objects': objects,
        'pdos': pdos,
        'pdo_rules': pdo_rules,
        'variants': variants,
        'dc_modes': device_dc_modes(newest),
        'commands': commands,
    }
    if supplement:
        apply_supplement(desc, supplement, obj_by_index)
    desc['warnings'] = warnings
    return desc


# --- Zusatzdatei -------------------------------------------------------------------------------


def parse_addr(key: str) -> tuple[int, int | None]:
    """"0xIIII:SS" bzw. "0xIIII" -> (index, sub oder None)."""
    idx, _, sub = key.partition(':')
    return int(idx, 16), int(sub, 16) if sub else None


def apply_supplement(desc: dict, sup: dict, obj_by_index: dict):
    """Führt die Zusatzdatei in die Beschreibung ein.

    - entries: {"0xIIII:SS" oder "0xIIII": {Felder..., "source"} oder Liste solcher Objekte (je
      Quelle eines)}; Felder werden in den Eintrag (bzw. alle Einträge des Objekts) übernommen
      und unter "supplement" aufgeführt.
    - commands: werden mit origin "supplement" angehängt.
    Vorhandene ESI-Felder dürfen nicht überschrieben werden.
    """
    items = [(k, f) for k, v in sorted(sup.get('entries', {}).items())
             for f in (v if isinstance(v, list) else [v])]
    for key, fields in items:
        if 'source' not in fields:
            raise EsiError(f'Zusatzdatei: {key} ohne source')
        idx, sub = parse_addr(key)
        obj = obj_by_index.get(idx)
        if obj is None:
            raise EsiError(f'Zusatzdatei: Objekt {key} nicht in der ESI')
        targets = [e for e in obj['entries'] if sub is None or e['sub'] == hex8(sub)]
        if not targets:
            raise EsiError(f'Zusatzdatei: Eintrag {key} nicht in der ESI')
        for e in targets:
            for k, v in fields.items():
                if k == 'source':
                    continue
                if k in e:
                    raise EsiError(f'Zusatzdatei: {key}.{k} steht bereits in der ESI')
                e[k] = v
            e.setdefault('supplement', []).append(
                {'fields': sorted(k for k in fields if k != 'source'), 'source': fields['source']})
    for c in sup.get('commands', []):
        if 'source' not in c:
            raise EsiError(f'Zusatzdatei: Kommando {c.get("id")} ohne source')
        desc['commands'].append({'origin': 'supplement', **c})
    ids = [c['id'] for c in desc['commands']]
    if len(ids) != len(set(ids)):
        raise EsiError('Kommando-IDs nicht eindeutig')


# --- Kommandozeile ---------------------------------------------------------------------------


def render(desc: dict) -> str:
    """JSON-Text (stabil, UTF-8, Zeilenende LF)."""
    return json.dumps(desc, ensure_ascii=False, indent=1) + '\n'


def main() -> int:
    """Kommandozeile; siehe Moduldokumentation.

    @return Exit-Code (0 ok, 1 Fehler bzw. Abweichung bei --check, 2 ungültige Optionen)
    """
    ap = argparse.ArgumentParser(description=__doc__.split('\n\n')[0])
    ap.add_argument('esi', type=Path, help='ESI-Datei')
    ap.add_argument('-t', '--type', required=True, metavar='TYP', help='Gerätetyp, z. B. EL3356-0010')
    ap.add_argument('-s', '--supplement', type=Path, help='Zusatzdatei (JSON)')
    ap.add_argument('-o', '--output', type=Path, required=True, help='Zieldatei (JSON)')
    ap.add_argument('--check', action='store_true',
                    help='nicht schreiben, nur mit der Zieldatei vergleichen')
    args = ap.parse_args()

    try:
        root = ET.fromstring(decode(args.esi.read_bytes()))
        devs = [d for d in root.iter('Device')
                if (d.findtext('Type') or '').strip().upper() == args.type.upper()]
        if not devs:
            raise EsiError(f'kein Gerät vom Typ {args.type}')
        sup = json.loads(args.supplement.read_text(encoding='utf-8')) if args.supplement else None
        out = render(build(devs, args.esi.name, root.find('Vendor').get('FileVersion'), sup,
                           args.supplement.name if args.supplement else None))
    except (OSError, ET.ParseError, UnicodeDecodeError, LookupError, ValueError, EsiError) as ex:
        print(f'{args.esi}: {ex}', file=sys.stderr)
        return 1

    if args.check:
        try:
            current = args.output.read_text(encoding='utf-8')
        except OSError as ex:
            print(f'{args.output}: {ex}', file=sys.stderr)
            return 1
        if current != out:
            print(f'{args.output} weicht von der Erzeugung aus {args.esi.name} ab; '
                  f'esi_gen.py ohne --check ausführen', file=sys.stderr)
            return 1
        return 0
    with open(args.output, 'w', encoding='utf-8', newline='\n') as f:
        f.write(out)
    print(f'{args.esi.name} ({args.type}, {len(devs)} Revisionen) -> {args.output}')
    return 0


if __name__ == '__main__':
    sys.exit(main())
