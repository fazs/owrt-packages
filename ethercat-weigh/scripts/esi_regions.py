#!/usr/bin/env python3
"""Bereitet Beckhoff-ESI-Dateien (EtherCATInfo) für die Ansicht in IntelliJ auf.

- Jedes <Device> wird in einen eigenen Faltbereich gelegt
  (<!--region ...--> / <!--endregion-->). Beschreibung: deutscher Name
  (Name LcId="1031", sonst der erste Name) und RevisionNo. IntelliJ zeigt sie
  im zugeklappten Zustand und unter Navigate -> Custom Folding (Strg+Alt+Punkt).
- Die Datei wird als UTF-8 geschrieben (XML-Deklaration angepasst).
- Optional: nur Geräte eines Typs behalten (--type EL3356 behält alle Revisionen
  von EL3356 und Varianten wie EL3356-0010).
- Optional: ImageData16x14-Elemente samt Daten entfernen (--strip-images).

Die Verarbeitung ist textbasiert; Formatierung, Kommentare und Zeilenenden
bleiben erhalten. Vorhandene Faltbereiche werden ersetzt, nicht verdoppelt.

Beispiele:
    esi_regions.py "Beckhoff EL33xx.xml" -o "EL3356.xml" --type EL3356 --strip-images
    esi_regions.py misc/Beckhoff-ESI/*.xml --in-place
"""

import argparse
import re
import sys
import xml.etree.ElementTree as ET
from pathlib import Path

# <Device ...> bis </Device> samt Einrückung; <Devices> und <DeviceType> werden durch \b
# bzw. (?=[\s>]) nicht erfasst. Device ist in EtherCATInfo nicht verschachtelt.
DEVICE_RE = re.compile(r'(?P<indent>[ \t]*)<Device(?=[\s>])[^>]*>.*?</Device>', re.DOTALL)
# Von diesem Skript erzeugte Faltbereiche (ganze Zeilen)
REGION_RE = re.compile(r'^[ \t]*<!--(?:region .*?|endregion)-->[ \t]*\r?\n', re.MULTILINE)
IMAGE_RE = re.compile(
    r'^[ \t]*<ImageData16x14(?:\s*/>|>.*?</ImageData16x14>)[ \t]*\r?\n?',
    re.DOTALL | re.MULTILINE,
)
DECL_RE = re.compile(r'^<\?xml[^>]*\?>')
ENC_RE = re.compile(r'''encoding\s*=\s*["']([^"']+)["']''')


def decode(raw: bytes) -> str:
    """Dekodiert gemäß XML-Deklaration (Default UTF-8).

    @param raw Dateiinhalt
    @return Text ohne BOM
    """
    head = raw[:200].decode('ascii', errors='replace')
    m = ENC_RE.search(head.split('?>', 1)[0])
    enc = m.group(1) if m else 'utf-8'
    return raw.decode(enc).lstrip('﻿')


def to_utf8_decl(text: str) -> str:
    """Setzt encoding="UTF-8" in der XML-Deklaration (bzw. ergänzt eine Deklaration).

    @param text Dokument
    @return Dokument mit UTF-8-Deklaration
    """
    m = DECL_RE.match(text)
    if not m:
        return '<?xml version="1.0" encoding="UTF-8"?>\n' + text
    decl = m.group(0)
    if ENC_RE.search(decl):
        new = ENC_RE.sub('encoding="UTF-8"', decl)
    else:
        new = decl[:-2].rstrip() + ' encoding="UTF-8"?>'
    return new + text[m.end():]


def device_info(block: str) -> tuple[str, str]:
    """Typ und Beschreibung eines Device-Blocks.

    @param block Text von <Device ...> bis </Device>
    @return (Type, Beschreibung für den Faltbereich)
    """
    dev = ET.fromstring(block)
    type_el = dev.find('Type')
    dev_type = (type_el.text or '').strip() if type_el is not None else ''
    rev = type_el.get('RevisionNo', '') if type_el is not None else ''
    names = dev.findall('Name')
    name = next((n.text for n in names if n.get('LcId') == '1031'), None)
    if name is None and names:
        name = names[0].text
    desc = ' '.join((name or dev_type or 'Device').split())
    if rev:
        desc += f' (Rev. {rev.replace("#x", "0x")})'
    # "--" ist in XML-Kommentaren nicht erlaubt
    return dev_type, desc.replace('--', '- -')


def matches_type(dev_type: str, wanted: str) -> bool:
    """Prüft, ob ein Gerät zum Filter gehört (exakt oder Variante mit "-").

    @param dev_type Inhalt von Device/Type, z. B. "EL3356-0010"
    @param wanted Filter, z. B. "EL3356"
    @return True für "EL3356" und "EL3356-xxxx", nicht für "EL33561"
    """
    a, b = dev_type.upper(), wanted.upper()
    return a == b or a.startswith(b + '-')


def process(text: str, wanted: str | None, strip_images: bool) -> tuple[str, int, int]:
    """Faltbereiche setzen, optional filtern und ImageData16x14 entfernen.

    @param text Dokument
    @param wanted Gerätetyp-Filter oder None
    @param strip_images ImageData16x14 entfernen
    @return (neues Dokument, behaltene Geräte, entfernte Geräte)
    """
    nl = '\r\n' if '\r\n' in text else '\n'
    text = REGION_RE.sub('', text)
    if strip_images:
        text = IMAGE_RE.sub('', text)

    kept = removed = 0

    def repl(m: re.Match) -> str:
        nonlocal kept, removed
        indent, block = m.group('indent'), m.group(0)[len(m.group('indent')):]
        dev_type, desc = device_info(block)
        if wanted and not matches_type(dev_type, wanted):
            removed += 1
            return '\0'  # Markierung: Zeile samt Umbruch entfernen
        kept += 1
        return f'{indent}<!--region {desc}-->{nl}{indent}{block}{nl}{indent}<!--endregion-->'

    text = DEVICE_RE.sub(repl, text)
    text = re.sub(r'\0[ \t]*\r?\n?', '', text)
    return to_utf8_decl(text), kept, removed


def main() -> int:
    """Kommandozeile; siehe Moduldokumentation.

    @return Exit-Code (0 ok, 1 Fehler, 2 ungültige Optionen)
    """
    ap = argparse.ArgumentParser(description=__doc__.split('\n\n')[0])
    ap.add_argument('files', nargs='+', type=Path, help='ESI-Datei(en)')
    out = ap.add_mutually_exclusive_group(required=True)
    out.add_argument('-o', '--output', type=Path, help='Zieldatei (nur bei einer Eingabedatei)')
    out.add_argument('-i', '--in-place', action='store_true', help='Eingabedateien überschreiben')
    ap.add_argument('-t', '--type', metavar='TYP',
                    help='nur Geräte dieses Typs behalten (inkl. Revisionen und Varianten TYP-xxxx)')
    ap.add_argument('--strip-images', action='store_true',
                    help='ImageData16x14-Elemente samt Daten entfernen')
    args = ap.parse_args()
    if args.output and len(args.files) != 1:
        ap.error('--output nur mit genau einer Eingabedatei')

    rc = 0
    for src in args.files:
        try:
            text, kept, removed = process(decode(src.read_bytes()), args.type, args.strip_images)
        except (OSError, ET.ParseError, UnicodeDecodeError, LookupError) as ex:
            print(f'{src}: {ex}', file=sys.stderr)
            rc = 1
            continue
        if args.type and kept == 0:
            print(f'{src}: kein Gerät vom Typ {args.type}; nicht geschrieben', file=sys.stderr)
            rc = 1
            continue
        dst = args.output or src
        # newline='' schreibt die vorhandenen Zeilenenden unverändert
        with open(dst, 'w', encoding='utf-8', newline='') as f:
            f.write(text)
        info = f'{kept} Geräte'
        if removed:
            info += f', {removed} entfernt'
        print(f'{src} -> {dst}: {info}')
    return rc


if __name__ == '__main__':
    sys.exit(main())
