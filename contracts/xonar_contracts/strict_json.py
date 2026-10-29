"""Analyse JSON stricte, avant toute validation de schéma.

Refuse ce que la bibliothèque standard accepte par défaut et que C3 interdit :
taille > 128 Kio, profondeur > 16, BOM ou UTF-8 invalide, clés dupliquées,
NaN/Infinity, nombres hors plage double, demi-paires de substitution isolées et
contenu après la valeur. Les entiers restent des entiers Python exacts (uint64
sans perte) ; un nombre à partie fractionnaire ou exposant reste un float, ce
qui rend « 1.0 » non entier pour le validateur.
"""

from __future__ import annotations

import json
import math
from typing import Any


class JsonError(Exception):
    def __init__(self, code: str, message: str, path: str = ""):
        super().__init__(f"{code} : {message}")
        self.code = code
        self.message = message
        self.path = path


def _depth_scan(text: str, max_depth: int) -> None:
    """Mesure l'imbrication sans récursion, en ignorant le contenu des chaînes."""
    depth = 0
    in_string = False
    escape = False
    for ch in text:
        if in_string:
            if escape:
                escape = False
            elif ch == "\\":
                escape = True
            elif ch == '"':
                in_string = False
            continue
        if ch == '"':
            in_string = True
        elif ch in "[{":
            depth += 1
            if depth > max_depth:
                raise JsonError("json.tooDeep", f"imbrication supérieure à {max_depth}")
        elif ch in "]}":
            depth -= 1


def _pairs(pairs: list[tuple[str, Any]]) -> dict[str, Any]:
    out: dict[str, Any] = {}
    for k, v in pairs:
        if k in out:
            raise JsonError("json.duplicateKey", f"clé dupliquée « {k} »")
        out[k] = v
    return out


def _float(s: str) -> float:
    v = float(s)
    if not math.isfinite(v):
        raise JsonError("json.nonFinite", f"nombre hors de la plage double : {s}")
    return v


def _constant(s: str) -> Any:
    raise JsonError("json.nonFinite", f"constante non finie interdite : {s}")


def _check_strings(value: Any, path: str = "") -> None:
    stack = [(value, path)]
    while stack:
        v, p = stack.pop()
        if isinstance(v, str):
            if any(0xD800 <= ord(c) <= 0xDFFF for c in v):
                raise JsonError("json.surrogate", "demi-paire de substitution isolée", p)
        elif isinstance(v, dict):
            for k, x in v.items():
                if any(0xD800 <= ord(c) <= 0xDFFF for c in k):
                    raise JsonError("json.surrogate", "demi-paire de substitution isolée dans une clé", p)
                stack.append((x, f"{p}/{k}"))
        elif isinstance(v, list):
            for i, x in enumerate(v):
                stack.append((x, f"{p}/{i}"))


def parse_strict(data: bytes, max_bytes: int | None = 131072, max_depth: int | None = 16) -> Any:
    if max_bytes is not None and len(data) > max_bytes:
        raise JsonError("json.tooLarge", f"{len(data)} octets > {max_bytes}")
    if data.startswith(b"\xef\xbb\xbf"):
        raise JsonError("json.encoding", "BOM UTF-8 interdit")
    try:
        text = data.decode("utf-8", errors="strict")
    except UnicodeDecodeError as exc:
        raise JsonError("json.encoding", f"UTF-8 invalide à l'octet {exc.start}") from None
    if max_depth is not None:
        _depth_scan(text, max_depth)
    decoder = json.JSONDecoder(object_pairs_hook=_pairs, parse_float=_float, parse_constant=_constant)
    try:
        value, end = decoder.raw_decode(text, _skip_ws(text, 0))
    except JsonError:
        raise
    except json.JSONDecodeError as exc:
        raise JsonError("json.syntax", f"{exc.msg} (ligne {exc.lineno}, colonne {exc.colno})") from None
    except RecursionError:
        raise JsonError("json.tooDeep", "imbrication excessive") from None
    except ValueError as exc:  # entier de plus de 4 300 chiffres, etc.
        raise JsonError("json.syntax", f"nombre illisible : {exc}") from None
    if _skip_ws(text, end) != len(text):
        raise JsonError("json.syntax", "contenu après la valeur JSON")
    _check_strings(value)
    return value


def _skip_ws(text: str, i: int) -> int:
    while i < len(text) and text[i] in " \t\r\n":
        i += 1
    return i
