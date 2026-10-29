"""Génération des schémas JSON Draft 2020-12 v1 depuis le manifeste.

Chaque fichier est autonome : toutes les définitions atteignables sont copiées
dans son $defs (copie générée, jamais dupliquée à la main). Seuls des mots-clés
standard sont émis, pour rester lisibles par n'importe quel validateur.
"""

from __future__ import annotations

from typing import Any

from .model import INT_RANGES, Manifest, Spec, Struct

DRAFT = "https://json-schema.org/draft/2020-12/schema"


def _scalar(spec: Spec) -> dict[str, Any]:
    c = spec.constraints
    k = spec.kind
    out: dict[str, Any]
    if k == "bool":
        out = {"type": "boolean"}
    elif k == "json":
        out = {"type": "object"}  # contenu libre, validé par le destinataire
    elif k == "string":
        out = {"type": "string"}
        if "minLength" in c:
            out["minLength"] = c["minLength"]
        if "maxLength" in c:
            out["maxLength"] = c["maxLength"]
        if "pattern" in c:
            out["pattern"] = c["pattern"]
    elif k == "f64":
        out = {"type": "number"}
        for src, dst in (("min", "minimum"), ("max", "maximum"),
                         ("exclusiveMin", "exclusiveMinimum"), ("exclusiveMax", "exclusiveMaximum")):
            if src in c:
                out[dst] = c[src]
    elif k in INT_RANGES:
        lo, hi = INT_RANGES[k]
        out = {"type": "integer", "minimum": max(lo, c.get("min", lo)), "maximum": min(hi, c.get("max", hi))}
    else:
        raise ValueError(k)
    if "const" in c:
        out["const"] = c["const"]
    if "allowed" in c:
        out["enum"] = list(c["allowed"])
    return out


class SchemaBuilder:
    def __init__(self, m: Manifest):
        self.m = m

    def spec(self, spec: Spec) -> dict[str, Any]:
        if spec.kind == "enum":
            if "const" in spec.constraints:
                return {"const": spec.constraints["const"]}
            return {"$ref": f"#/$defs/{spec.ref}"}
        if spec.kind in ("struct", "union"):
            return {"$ref": f"#/$defs/{spec.ref}"}
        if spec.kind == "array":
            out: dict[str, Any] = {"type": "array", "items": self.spec(spec.item)}
            a = spec.array
            if "length" in a:
                out["minItems"] = a["length"]
                out["maxItems"] = a["length"]
            if "minItems" in a:
                out["minItems"] = a["minItems"]
            if "maxItems" in a:
                out["maxItems"] = a["maxItems"]
            if a.get("uniqueItems"):
                out["uniqueItems"] = True
            return out
        if spec.kind == "switch":
            return {"type": "object"}
        return _scalar(spec)

    def _cond_values(self, st: Struct, rule: dict[str, Any]) -> list[str]:
        disc = st.field(rule["field"])
        en = self.m.enums[disc.spec.ref]
        if "attr" in rule:
            return [v.wire for v in en.values if v.attrs[rule["attr"]] in rule["in"]]
        return list(rule["in"])

    @staticmethod
    def _effect(name: str, effect: str) -> dict[str, Any]:
        if effect == "required":
            return {"required": [name]}
        if effect == "nonNull":
            return {"properties": {name: {"not": {"type": "null"}}}}
        if effect == "null":
            return {"properties": {name: {"type": "null"}}}
        raise ValueError(effect)

    def struct(self, st: Struct) -> dict[str, Any]:
        props: dict[str, Any] = {}
        required: list[str] = []
        all_of: list[dict[str, Any]] = []
        for f in st.fields:
            s = self.spec(f.spec)
            if f.nullable:
                s = {"anyOf": [s, {"type": "null"}]}
            if f.doc:
                s = {"description": f.doc, **s}
            if f.has_default and not isinstance(f.default, dict) and f.default != "$itemDefaults":
                s["default"] = f.default
            props[f.name] = s
            if f.required:
                required.append(f.name)
            for rule in f.when:
                cond = {"properties": {rule["field"]: {"enum": self._cond_values(st, rule)}},
                        "required": [rule["field"]]}
                entry: dict[str, Any] = {"if": cond, "then": self._effect(f.name, rule["then"])}
                if "else" in rule:
                    entry["else"] = self._effect(f.name, rule["else"])
                all_of.append(entry)
            if f.spec.kind == "switch":
                for wire, case in f.spec.cases.items():
                    all_of.append({
                        "if": {"properties": {f.spec.switch_field: {"const": wire}},
                               "required": [f.spec.switch_field]},
                        "then": {"properties": {f.name: self.spec(case)}},
                    })
        out: dict[str, Any] = {"type": "object"}
        if st.doc:
            out = {"description": st.doc, **out}
        out["additionalProperties"] = False
        out["required"] = required
        out["properties"] = props
        if all_of:
            out["allOf"] = all_of
        return out

    def definition(self, name: str) -> dict[str, Any]:
        m = self.m
        if name in m.enums:
            e = m.enums[name]
            return {"description": e.doc, "type": "string", "enum": e.wires}
        if name in m.unions:
            u = m.unions[name]
            return {"description": u.doc, "oneOf": [{"$ref": f"#/$defs/{x}"} for x in u.members]}
        return self.struct(m.structs[name])

    def schema_file(self, key: str) -> dict[str, Any]:
        info = self.m.schemas[key]
        roots = info["root"]
        doc: dict[str, Any] = {
            "$schema": DRAFT,
            "$id": info["id"],
            "title": info["title"],
            "description": info["description"],
            "$comment": f"Généré par contracts/generate.py depuis contracts/manifest.json (sha256 {self.m.sha256}). Ne pas éditer.",
        }
        if len(roots) == 1:
            doc["$ref"] = f"#/$defs/{roots[0]}"
        else:
            doc["oneOf"] = [{"$ref": f"#/$defs/{r}"} for r in roots]
        names = self.m.reachable(list(info["entries"]))
        doc["$defs"] = {n: self.definition(n) for n in names}
        return doc


def build_schemas(m: Manifest) -> dict[str, dict[str, Any]]:
    b = SchemaBuilder(m)
    return {key: b.schema_file(key) for key in m.schemas}
