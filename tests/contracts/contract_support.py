"""Aides communes aux tests des contrats (bibliothèque standard seule)."""

from __future__ import annotations

import copy
import json
import sys
from pathlib import Path
from typing import Any

HERE = Path(__file__).resolve().parent
REPO = HERE.parents[1]
CONTRACTS = REPO / "contracts"
if str(CONTRACTS) not in sys.path:
    sys.path.insert(0, str(CONTRACTS))

from xonar_contracts.model import canonical_json, load_manifest  # noqa: E402
from xonar_contracts.strict_json import parse_strict  # noqa: E402

NEGATIVE_CASES = HERE / "negative_cases.json"


def manifest():
    return load_manifest()


def load_json(path: Path) -> Any:
    return parse_strict(path.read_bytes(), max_bytes=None, max_depth=None)


def fixture_targets() -> list[tuple[Path, str]]:
    """(fichier, cible) de chaque fixture positive déclarée par le manifeste."""
    out: list[tuple[Path, str]] = []
    for fx in manifest().fixtures:
        d = REPO / fx["dir"]
        for f in sorted(d.glob("*.json")):
            out.append((f, fx["target"]))
    return out


def _resolve(value: Any) -> Any:
    if isinstance(value, dict) and set(value) == {"$repeat"}:
        text, n = value["$repeat"]
        return text * n
    return value


def _tokens(pointer: str) -> list[str]:
    return [t.replace("~1", "/").replace("~0", "~") for t in pointer.split("/")[1:]]


def _parent(doc: Any, pointer: str) -> tuple[Any, str]:
    toks = _tokens(pointer)
    node = doc
    for t in toks[:-1]:
        node = node[int(t)] if isinstance(node, list) else node[t]
    return node, toks[-1]


def apply_set(doc: Any, pointer: str, value: Any) -> None:
    node, last = _parent(doc, pointer)
    value = _resolve(copy.deepcopy(value))
    if isinstance(node, list):
        if last == "-":
            node.append(value)
        else:
            node[int(last)] = value
    else:
        node[last] = value


def apply_remove(doc: Any, pointer: str) -> None:
    node, last = _parent(doc, pointer)
    if isinstance(node, list):
        del node[int(last)]
    else:
        del node[last]


def negative_cases() -> dict[str, Any]:
    return load_json(NEGATIVE_CASES)


def build_case(spec: dict[str, Any], case: dict[str, Any]) -> tuple[bytes, str]:
    """Construit les octets du cas et sa cible de validation."""
    if "raw" in case:
        return case["raw"].encode("utf-8"), case["target"]
    base_key = case["base"]
    path = REPO / spec["bases"][base_key]
    target = case.get("target", spec["targets"][base_key])
    data = path.read_bytes()
    if "set" in case or "remove" in case:
        doc = load_json(path)
        for p in case.get("remove", []):
            apply_remove(doc, p)
        for p, v in case.get("set", {}).items():
            apply_set(doc, p, v)
        data = canonical_json(doc).encode("utf-8")
    for old, new in case.get("replace", []):
        text = data.decode("utf-8")
        if text.count(old) != 1:
            raise AssertionError(f"{case['id']} : « {old} » doit apparaître une seule fois dans {path.name}")
        data = text.replace(old, new).encode("utf-8")
    if case.get("invalidUtf8"):
        if data.count(b'"speakers"') != 1:
            raise AssertionError(f"{case['id']} : ancre absente")
        data = data.replace(b'"speakers"', b'"speak\xffers"')
    if case.get("prefixBom"):
        data = b"\xef\xbb\xbf" + data
    if "padTo" in case:
        n = case["padTo"]
        data = data + b" " * (n - len(data))
        if len(data) != n:
            raise AssertionError(f"{case['id']} : base déjà plus grande que {n} octets")
    return data, target


def dump(value: Any) -> str:
    return json.dumps(value, ensure_ascii=False)
