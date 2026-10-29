"""Normalisation : remplit les champs et groupes absents avec les défauts C++.

Les défauts proviennent du manifeste, eux-mêmes vérifiés contre
xonar::dsp::Profile par le test de conformité C++ généré. L'ordre des clés
produit est celui du manifeste ; la fonction est idempotente.
"""

from __future__ import annotations

import copy
from typing import Any

from .model import Manifest, Spec


def default_value(m: Manifest, spec: Spec, default: Any) -> Any:
    if spec.kind == "struct" and isinstance(default, dict):
        return normalize(m, spec.ref, default)
    if spec.kind == "array" and default == "$itemDefaults":
        return [_item_default(m, spec.item) for _ in range(spec.array["length"])]
    return copy.deepcopy(default)


def _item_default(m: Manifest, spec: Spec) -> Any:
    if spec.kind == "struct":
        return normalize(m, spec.ref, {})
    if spec.kind == "array":
        return [_item_default(m, spec.item) for _ in range(spec.array["length"])]
    raise ValueError("défaut d'élément non défini pour un scalaire")


def _value(m: Manifest, spec: Spec, v: Any) -> Any:
    if v is None:
        return None
    if spec.kind == "struct":
        return normalize(m, spec.ref, v)
    if spec.kind == "union":
        u = m.unions[spec.ref]
        for member in u.members:
            const = m.structs[member].field(u.discriminator).spec.constraints["const"]
            if isinstance(v, dict) and v.get(u.discriminator) == const:
                return normalize(m, member, v)
        return copy.deepcopy(v)
    if spec.kind == "array" and isinstance(v, list):
        return [_value(m, spec.item, x) for x in v]
    return copy.deepcopy(v)


def normalize(m: Manifest, struct_id: str, doc: dict[str, Any]) -> dict[str, Any]:
    """Retourne une copie complète ; les champs inconnus sont conservés à la fin
    (la validation de schéma les refuse, la normalisation ne masque rien)."""
    st = m.structs[struct_id]
    out: dict[str, Any] = {}
    for f in st.fields:
        if f.name in doc:
            if f.spec.kind == "switch":
                disc = doc.get(f.spec.switch_field)
                case = f.spec.cases.get(disc)
                out[f.name] = _value(m, case, doc[f.name]) if case else copy.deepcopy(doc[f.name])
            else:
                out[f.name] = _value(m, f.spec, doc[f.name])
        elif f.has_default:
            out[f.name] = default_value(m, f.spec, f.default)
    for k, v in doc.items():
        if k not in out:
            out[k] = copy.deepcopy(v)
    return out
