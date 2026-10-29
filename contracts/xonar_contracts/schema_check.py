"""Validateur JSON Schema Draft 2020-12 (sous-ensemble), bibliothèque standard seule.

Couvre exactement les mots-clés émis par gen_schema et ceux du schéma v0.
Tout mot-clé inconnu dans un schéma lève SchemaUnsupported : rien n'est ignoré
silencieusement. Écarts volontaires, plus stricts que la norme :
- « integer » exige un entier littéral JSON (1.0 est refusé) ;
- booléen et nombre ne sont jamais égaux pour const/enum/uniqueItems.
"""

from __future__ import annotations

import re
from dataclasses import dataclass
from typing import Any

ANNOTATIONS = {"$schema", "$id", "title", "description", "$comment", "default", "$defs"}
KEYWORDS = ANNOTATIONS | {
    "type", "enum", "const", "minimum", "maximum", "exclusiveMinimum", "exclusiveMaximum",
    "minLength", "maxLength", "pattern", "items", "minItems", "maxItems", "uniqueItems",
    "properties", "required", "additionalProperties", "$ref", "allOf", "anyOf", "oneOf",
    "not", "if", "then", "else",
}


class SchemaUnsupported(Exception):
    pass


@dataclass(frozen=True)
class Issue:
    code: str
    path: str
    message: str

    def __str__(self) -> str:
        return f"{self.code} {self.path or '/'} : {self.message}"


def json_type(v: Any) -> str:
    if v is None:
        return "null"
    if isinstance(v, bool):
        return "boolean"
    if isinstance(v, int):
        return "integer"
    if isinstance(v, float):
        return "number"
    if isinstance(v, str):
        return "string"
    if isinstance(v, list):
        return "array"
    if isinstance(v, dict):
        return "object"
    raise TypeError(type(v))


def json_equal(a: Any, b: Any) -> bool:
    ta, tb = json_type(a), json_type(b)
    num = {"integer", "number"}
    if ta in num and tb in num:
        return a == b
    if ta != tb:
        return False
    if ta == "array":
        return len(a) == len(b) and all(json_equal(x, y) for x, y in zip(a, b))
    if ta == "object":
        return a.keys() == b.keys() and all(json_equal(a[k], b[k]) for k in a)
    return a == b


def _type_ok(v: Any, t: str) -> bool:
    jt = json_type(v)
    if t == "number":
        return jt in ("integer", "number")
    return jt == t


def _esc(token: str) -> str:
    return token.replace("~", "~0").replace("/", "~1")


class Validator:
    def __init__(self, root: dict[str, Any]):
        self.root = root
        self._re: dict[str, re.Pattern[str]] = {}
        self._check_keywords(root)

    def _check_keywords(self, s: Any) -> None:
        if isinstance(s, bool):
            return
        if not isinstance(s, dict):
            raise SchemaUnsupported(f"schéma non objet : {s!r}")
        unknown = set(s) - KEYWORDS
        if unknown:
            raise SchemaUnsupported(f"mots-clés non pris en charge : {sorted(unknown)}")
        for k in ("items", "additionalProperties", "not", "if", "then", "else"):
            if k in s:
                self._check_keywords(s[k])
        for k in ("properties", "$defs"):
            for sub in s.get(k, {}).values():
                self._check_keywords(sub)
        for k in ("allOf", "anyOf", "oneOf"):
            for sub in s.get(k, []):
                self._check_keywords(sub)

    def resolve(self, ref: str) -> Any:
        if not ref.startswith("#"):
            raise SchemaUnsupported(f"référence externe non prise en charge : {ref}")
        node: Any = self.root
        for tok in [t for t in ref[1:].split("/") if t]:
            node = node[tok.replace("~1", "/").replace("~0", "~")]
        return node

    def validate(self, value: Any, schema: Any = None, path: str = "") -> list[Issue]:
        return self._v(value, self.root if schema is None else schema, path)

    @staticmethod
    def _closest(results: list[list[Issue]], p: str) -> list[Issue]:
        """Erreurs de l'alternative la plus proche, pour un diagnostic utile : on
        écarte d'abord celles dont le type ou le discriminant (const d'un champ
        direct) ne correspond pas, puis on retient la moins fautive."""
        def score(r: list[Issue]) -> tuple[bool, bool, int]:
            wrong_type = any(i.code == "schema.type" and i.path == p for i in r)
            wrong_disc = any(i.code == "schema.const" and i.path.rsplit("/", 1)[0] == p for i in r)
            return wrong_type, wrong_disc, len(r)
        return min(results, key=score)

    def _v(self, v: Any, s: Any, p: str) -> list[Issue]:
        if s is True:
            return []
        if s is False:
            return [Issue("schema.false", p, "valeur interdite")]
        out: list[Issue] = []
        if "$ref" in s:
            out += self._v(v, self.resolve(s["$ref"]), p)
        if "type" in s:
            types = s["type"] if isinstance(s["type"], list) else [s["type"]]
            if not any(_type_ok(v, t) for t in types):
                return out + [Issue("schema.type", p, f"type {json_type(v)} au lieu de {'/'.join(types)}")]
        if "const" in s and not json_equal(v, s["const"]):
            out.append(Issue("schema.const", p, f"valeur {v!r} au lieu de {s['const']!r}"))
        if "enum" in s and not any(json_equal(v, e) for e in s["enum"]):
            out.append(Issue("schema.enum", p, f"valeur {v!r} hors de {s['enum']!r}"))
        jt = json_type(v)
        if jt in ("integer", "number"):
            if "minimum" in s and v < s["minimum"]:
                out.append(Issue("schema.minimum", p, f"{v} < {s['minimum']}"))
            if "maximum" in s and v > s["maximum"]:
                out.append(Issue("schema.maximum", p, f"{v} > {s['maximum']}"))
            if "exclusiveMinimum" in s and v <= s["exclusiveMinimum"]:
                out.append(Issue("schema.exclusiveMinimum", p, f"{v} ≤ {s['exclusiveMinimum']}"))
            if "exclusiveMaximum" in s and v >= s["exclusiveMaximum"]:
                out.append(Issue("schema.exclusiveMaximum", p, f"{v} ≥ {s['exclusiveMaximum']}"))
        if jt == "string":
            if "minLength" in s and len(v) < s["minLength"]:
                out.append(Issue("schema.minLength", p, f"longueur {len(v)} < {s['minLength']}"))
            if "maxLength" in s and len(v) > s["maxLength"]:
                out.append(Issue("schema.maxLength", p, f"longueur {len(v)} > {s['maxLength']}"))
            if "pattern" in s:
                rx = self._re.setdefault(s["pattern"], re.compile(s["pattern"]))
                if not rx.search(v):
                    out.append(Issue("schema.pattern", p, f"« {v[:40]} » ne respecte pas {s['pattern']}"))
        if jt == "array":
            if "minItems" in s and len(v) < s["minItems"]:
                out.append(Issue("schema.minItems", p, f"{len(v)} éléments < {s['minItems']}"))
            if "maxItems" in s and len(v) > s["maxItems"]:
                out.append(Issue("schema.maxItems", p, f"{len(v)} éléments > {s['maxItems']}"))
            if s.get("uniqueItems"):
                for i in range(len(v)):
                    if any(json_equal(v[i], v[j]) for j in range(i)):
                        out.append(Issue("schema.uniqueItems", f"{p}/{i}", "élément dupliqué"))
            if "items" in s:
                for i, x in enumerate(v):
                    out += self._v(x, s["items"], f"{p}/{i}")
        if jt == "object":
            for r in s.get("required", []):
                if r not in v:
                    out.append(Issue("schema.required", f"{p}/{_esc(r)}", f"champ obligatoire « {r} » absent"))
            props = s.get("properties", {})
            for k, x in v.items():
                if k in props:
                    out += self._v(x, props[k], f"{p}/{_esc(k)}")
                elif "additionalProperties" in s:
                    ap = s["additionalProperties"]
                    if ap is False:
                        out.append(Issue("schema.additionalProperties", f"{p}/{_esc(k)}", f"champ inconnu « {k} »"))
                    else:
                        out += self._v(x, ap, f"{p}/{_esc(k)}")
        for sub in s.get("allOf", []):
            out += self._v(v, sub, p)
        if "anyOf" in s:
            results = [self._v(v, sub, p) for sub in s["anyOf"]]
            if not any(not r for r in results):
                out.append(Issue("schema.anyOf", p, "aucune alternative valide"))
                out += self._closest(results, p)
        if "oneOf" in s:
            results = [self._v(v, sub, p) for sub in s["oneOf"]]
            ok = sum(1 for r in results if not r)
            if ok == 0:
                out.append(Issue("schema.oneOf", p, "aucune alternative valide"))
                out += self._closest(results, p)
            elif ok > 1:
                out.append(Issue("schema.oneOf", p, f"{ok} alternatives valides au lieu d'une"))
        if "not" in s and not self._v(v, s["not"], p):
            out.append(Issue("schema.not", p, "valeur interdite"))
        if "if" in s:
            if not self._v(v, s["if"], p):
                if "then" in s:
                    out += self._v(v, s["then"], p)
            elif "else" in s:
                out += self._v(v, s["else"], p)
        return out
