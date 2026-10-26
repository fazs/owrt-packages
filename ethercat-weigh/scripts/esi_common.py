"""Gemeinsame Hilfsfunktionen für die ESI-Skripte (esi_regions.py, esi_gen.py)."""

import re

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


def matches_type(dev_type: str, wanted: str) -> bool:
    """Prüft, ob ein Gerät zum Filter gehört (exakt oder Variante mit "-").

    @param dev_type Inhalt von Device/Type, z. B. "EL3356-0010"
    @param wanted Filter, z. B. "EL3356"
    @return True für "EL3356" und "EL3356-xxxx", nicht für "EL33561"
    """
    a, b = dev_type.upper(), wanted.upper()
    return a == b or a.startswith(b + '-')
