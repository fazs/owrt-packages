"""Common helper functions for the ESI scripts (esi_regions.py, esi_gen.py)."""

import re

ENC_RE = re.compile(r'''encoding\s*=\s*["']([^"']+)["']''')


def decode(raw: bytes) -> str:
    """Decodes according to the XML declaration (default UTF-8).

    @param raw file content
    @return text without BOM
    """
    head = raw[:200].decode('ascii', errors='replace')
    m = ENC_RE.search(head.split('?>', 1)[0])
    enc = m.group(1) if m else 'utf-8'
    return raw.decode(enc).lstrip('﻿')


def matches_type(dev_type: str, wanted: str) -> bool:
    """Checks whether a device matches the filter (exact or variant with "-").

    @param dev_type content of Device/Type, e.g. "EL3356-0010"
    @param wanted filter, e.g. "EL3356"
    @return True for "EL3356" and "EL3356-xxxx", not for "EL33561"
    """
    a, b = dev_type.upper(), wanted.upper()
    return a == b or a.startswith(b + '-')
