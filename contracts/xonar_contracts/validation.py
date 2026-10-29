"""Chaîne de validation complète d'un document v1.

octets → taille / encodage / profondeur → JSON strict → version majeure →
schéma v1 (fichier versionné dans configs/profiles) → règles sémantiques.
Une version majeure inconnue arrête la chaîne : le document n'est jamais
interprété approximativement.

Cible : « profile-v1 », « control-v1 », « state-v1 » (racine du schéma) ou
« <schéma>#<Définition> » (ex. « profile-v1#CapxDocument »).
"""

from __future__ import annotations

from functools import lru_cache
from typing import Any

from .model import REPO_ROOT, Manifest, load_manifest
from .schema_check import Issue, Validator
from .semantic import check
from .strict_json import JsonError, parse_strict


@lru_cache(maxsize=1)
def manifest() -> Manifest:
    return load_manifest()


@lru_cache(maxsize=8)
def _validator(key: str) -> Validator:
    m = manifest()
    path = REPO_ROOT / m.schemas[key]["file"]
    return Validator(parse_strict(path.read_bytes(), max_bytes=None, max_depth=None))


def _split(target: str) -> tuple[str, list[str], bool]:
    m = manifest()
    key, _, definition = target.partition("#")
    if key not in m.schemas:
        raise ValueError(f"cible inconnue {target}")
    if definition:
        if definition not in m.schemas[key]["entries"]:
            raise ValueError(f"{definition} n'est pas une entrée de {key}")
        return key, [definition], True
    return key, list(m.schemas[key]["root"]), False


def _version(m: Manifest, candidates: list[str], value: Any) -> list[Issue]:
    if not isinstance(value, dict):
        return []
    for sid in candidates:
        vf = m.structs[sid].version_field
        if vf and vf["name"] in value:
            v = value[vf["name"]]
            p = "/" + vf["name"]
            if isinstance(v, bool) or not isinstance(v, int):
                return [Issue("version.invalid", p, f"version {v!r} : entier littéral attendu")]
            if v != vf["supported"]:
                return [Issue("version.unsupportedMajor", p, f"version majeure {v} inconnue (prise en charge : {vf['supported']})")]
            return []
    return []


def validate_value(value: Any, target: str, context: dict[str, Any] | None = None) -> list[Issue]:
    m = manifest()
    key, candidates, is_def = _split(target)
    issues = _version(m, candidates, value)
    if issues:
        return issues
    v = _validator(key)
    if is_def:
        issues = v.validate(value, {"$ref": f"#/$defs/{candidates[0]}"})
    else:
        issues = v.validate(value)
    if issues:
        return issues
    for sid in candidates:
        if not v.validate(value, {"$ref": f"#/$defs/{sid}"}):
            return check(m, sid, value, context)
    return []


def validate_bytes(data: bytes, target: str, context: dict[str, Any] | None = None) -> list[Issue]:
    m = manifest()
    try:
        value = parse_strict(data, max_bytes=m.const("MaxDocumentBytes"), max_depth=m.const("MaxDocumentDepth"))
    except JsonError as exc:
        return [Issue(exc.code, exc.path, exc.message)]
    return validate_value(value, target, context)


def matched_root(value: Any, target: str) -> str | None:
    """Définition racine reconnue (utile pour la cible multi-racines control-v1)."""
    key, candidates, _ = _split(target)
    v = _validator(key)
    for sid in candidates:
        if not v.validate(value, {"$ref": f"#/$defs/{sid}"}):
            return sid
    return None
