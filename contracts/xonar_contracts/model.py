"""Chargement et résolution du manifeste des contrats (contracts/manifest.json).

Le manifeste est la source unique : constantes, GUID, énumérations, types et
règles. Ce module le transforme en un modèle typé, résout les références
« *From » vers les constantes et vérifie sa cohérence interne. Aucune valeur
n'est inventée ici ; toute incohérence lève ManifestError.
"""

from __future__ import annotations

import hashlib
import json
import re
from dataclasses import dataclass, field
from pathlib import Path
from typing import Any

REPO_ROOT = Path(__file__).resolve().parents[2]
MANIFEST_PATH = REPO_ROOT / "contracts" / "manifest.json"

SCALARS = {"bool", "f64", "u8", "u16", "u32", "u64", "i32", "string", "json"}
INT_RANGES = {
    "u8": (0, 2**8 - 1),
    "u16": (0, 2**16 - 1),
    "u32": (0, 2**32 - 1),
    "u64": (0, 2**64 - 1),
    "i32": (-(2**31), 2**31 - 1),
}
CONSTRAINT_KEYS = (
    "min", "max", "exclusiveMin", "exclusiveMax", "const", "allowed",
    "pattern", "minLength", "maxLength",
)
ARRAY_KEYS = ("length", "minItems", "maxItems")
GUID_RE = re.compile(r"^[0-9A-F]{8}-[0-9A-F]{4}-4[0-9A-F]{3}-[89AB][0-9A-F]{3}-[0-9A-F]{12}$")
IDENT_RE = re.compile(r"^[A-Z][A-Za-z0-9]*$")
FIELD_RE = re.compile(r"^[a-z][a-z0-9]*([A-Z][a-z0-9]*)*$|^[a-z]$")


class ManifestError(Exception):
    """Manifeste incohérent."""


@dataclass
class Spec:
    """Spécification d'un type de valeur."""

    kind: str  # scalaire, "enum", "struct", "union", "array", "switch"
    ref: str | None = None
    constraints: dict[str, Any] = field(default_factory=dict)
    item: "Spec | None" = None
    array: dict[str, Any] = field(default_factory=dict)
    switch_field: str | None = None
    switch_base: str | None = None
    cases: dict[str, "Spec"] = field(default_factory=dict)


@dataclass
class Field:
    name: str
    spec: Spec
    required: bool
    nullable: bool
    has_default: bool
    default: Any
    doc: str
    dsp: str | None
    nyquist: bool
    when: list[dict[str, Any]]


@dataclass
class EnumValue:
    id: str
    wire: str
    index: int
    attrs: dict[str, Any]
    doc: str


@dataclass
class Enum:
    id: str
    contract: str
    doc: str
    emit: bool
    attrs: dict[str, str]
    values: list[EnumValue]
    mirror: dict[str, Any] | None = None

    def by_wire(self, wire: str) -> EnumValue:
        for v in self.values:
            if v.wire == wire:
                return v
        raise ManifestError(f"valeur « {wire} » absente de l'énumération {self.id}")

    @property
    def wires(self) -> list[str]:
        return [v.wire for v in self.values]


@dataclass
class Struct:
    id: str
    contract: str
    doc: str
    fields: list[Field]
    version_field: dict[str, Any] | None

    def field(self, name: str) -> Field:
        for f in self.fields:
            if f.name == name:
                return f
        raise KeyError(name)


@dataclass
class Union:
    id: str
    contract: str
    doc: str
    discriminator: str
    members: list[str]


@dataclass
class Constant:
    id: str
    type: Any
    value: Any
    contract: str
    doc: str
    mirror: str | None = None


class Manifest:
    def __init__(self, raw: dict[str, Any], raw_bytes: bytes):
        self.raw = raw
        # Empreinte calculée sur un contenu aux fins de ligne LF : indépendante
        # de la conversion éventuelle CRLF de l'extraction Git.
        self.sha256 = hashlib.sha256(raw_bytes.replace(b"\r\n", b"\n")).hexdigest()
        self.contracts = raw["contracts"]
        self.constants: dict[str, Constant] = {}
        for c in raw["constants"]:
            if c["id"] in self.constants:
                raise ManifestError(f"constante dupliquée {c['id']}")
            self.constants[c["id"]] = Constant(c["id"], c["type"], c["value"], c["contract"], c.get("doc", ""),
                                               c.get("cppMirror"))
        self.guids = {g["id"]: g for g in raw["guids"]}
        if len(self.guids) != len(raw["guids"]):
            raise ManifestError("identifiant de GUID dupliqué")
        self.property_keys = raw["propertyKeys"]
        self.enums: dict[str, Enum] = {}
        for e in raw["enums"]:
            values = [
                EnumValue(v["id"], v["wire"], i, v.get("attrs", {}), v.get("doc", ""))
                for i, v in enumerate(e["values"])
            ]
            self.enums[e["id"]] = Enum(e["id"], e["contract"], e.get("doc", ""), e.get("emit", True),
                                       e.get("attrs", {}), values, e.get("cppMirror"))
        self.unions: dict[str, Union] = {
            u["id"]: Union(u["id"], u["contract"], u.get("doc", ""), u["discriminator"], list(u["members"]))
            for u in raw["unions"]
        }
        self.structs: dict[str, Struct] = {}
        for t in raw["types"]:
            if t["id"] in self.structs:
                raise ManifestError(f"type dupliqué {t['id']}")
            self.structs[t["id"]] = Struct(t["id"], t["contract"], t.get("doc", ""), [], t.get("versionField"))
        for t in raw["types"]:
            self.structs[t["id"]].fields = [self._parse_field(t["id"], f) for f in t["fields"]]
        self.semantic_rules = raw["semanticRules"]
        self.schemas = {s["key"]: s for s in raw["schemas"]}
        self.fixtures = raw["fixtures"]
        self.outputs = raw["outputs"]
        self._check()

    # ------------------------------------------------------------------ util
    def const(self, cid: str) -> Any:
        if cid not in self.constants:
            raise ManifestError(f"constante inconnue {cid}")
        return self.constants[cid].value

    def _parse_spec(self, owner: str, obj: Any, extra: dict[str, Any] | None = None) -> Spec:
        extra = dict(extra or {})
        if isinstance(obj, dict) and "array" in obj:
            item = self._parse_spec(owner, obj["array"])
            arr: dict[str, Any] = {}
            for k in ARRAY_KEYS:
                if k in obj:
                    arr[k] = obj[k]
                if k + "From" in obj:
                    arr[k] = self.const(obj[k + "From"])
            if obj.get("uniqueItems"):
                arr["uniqueItems"] = True
            return Spec("array", item=item, array=arr)
        if isinstance(obj, dict) and "switch" in obj:
            cases = {k: self._parse_spec(owner, v) for k, v in obj["cases"].items()}
            return Spec("switch", switch_field=obj["switch"], switch_base=obj["base"], cases=cases)
        if isinstance(obj, dict):
            extra.update({k: v for k, v in obj.items() if k != "type"})
            obj = obj["type"]
        if not isinstance(obj, str):
            raise ManifestError(f"{owner} : type illisible {obj!r}")
        constraints: dict[str, Any] = {}
        for k in CONSTRAINT_KEYS:
            if k in extra:
                constraints[k] = extra[k]
            if k + "From" in extra:
                constraints[k] = self.const(extra[k + "From"])
        if obj in SCALARS:
            return Spec(obj, constraints=constraints)
        prefix, _, name = obj.partition(":")
        if prefix == "enum":
            return Spec("enum", ref=name, constraints=constraints)
        if prefix == "type":
            return Spec("struct", ref=name)
        if prefix == "union":
            return Spec("union", ref=name)
        raise ManifestError(f"{owner} : type inconnu {obj}")

    def _parse_field(self, owner: str, f: dict[str, Any]) -> Field:
        name = f["name"]
        if not FIELD_RE.match(name):
            raise ManifestError(f"{owner}.{name} : nom de champ non camelCase")
        extra = {k: v for k, v in f.items() if k not in ("name", "type")}
        spec = self._parse_spec(f"{owner}.{name}", f["type"], extra)
        has_default = "default" in f or "defaultFrom" in f
        default = self.const(f["defaultFrom"]) if "defaultFrom" in f else f.get("default")
        return Field(
            name=name,
            spec=spec,
            required=f.get("required", True),
            nullable=f.get("nullable", False),
            has_default=has_default,
            default=default,
            doc=f.get("doc", ""),
            dsp=f.get("dsp"),
            nyquist=bool(f.get("nyquist", False)),
            when=list(f.get("when", [])),
        )

    # ----------------------------------------------------------- cohérence
    def _check(self) -> None:
        ids = [c["id"] for c in self.contracts]
        if ids != [f"C{i}" for i in range(1, 9)]:
            raise ManifestError("les contrats doivent être exactement C1…C8 dans l'ordre")
        values = [g["value"] for g in self.guids.values()]
        if len(set(values)) != len(values):
            raise ManifestError("valeur de GUID dupliquée")
        for g in self.guids.values():
            if not GUID_RE.match(g["value"]):
                raise ManifestError(f"GUID {g['id']} non canonique (v4 majuscules attendu)")
            if g.get("assignedBy") != "E0-02":
                raise ManifestError(f"GUID {g['id']} non attribué par E0-02")
        for pk in self.property_keys:
            if pk["fmtid"] not in self.guids:
                raise ManifestError(f"clé {pk['id']} : FMTID inconnu")
        pids = [(pk["fmtid"], pk["pid"]) for pk in self.property_keys]
        if len(set(pids)) != len(pids):
            raise ManifestError("PID de propriété dupliqué")
        names = list(self.enums) + list(self.structs) + list(self.unions)
        if len(set(names)) != len(names):
            raise ManifestError("collision de noms entre énumérations, types et unions")
        for n in names:
            if not IDENT_RE.match(n):
                raise ManifestError(f"identifiant {n} non PascalCase")
        for e in self.enums.values():
            wires = e.wires
            if len(set(wires)) != len(wires) or len({v.id for v in e.values}) != len(e.values):
                raise ManifestError(f"énumération {e.id} : valeur dupliquée")
            for v in e.values:
                if not IDENT_RE.match(v.id):
                    raise ManifestError(f"{e.id}.{v.id} non PascalCase")
                if set(v.attrs) != set(e.attrs):
                    raise ManifestError(f"{e.id}.{v.id} : attributs incomplets")
                for an, at in e.attrs.items():
                    if at.startswith("enum:"):
                        self.enums[at[5:]].by_wire(v.attrs[an])
        for c in self.constants.values():
            self._check_constant(c)
        for u in self.unions.values():
            for m in u.members:
                st = self.structs[m]
                d = st.field(u.discriminator)
                if d.spec.kind != "enum" or "const" not in d.spec.constraints:
                    raise ManifestError(f"union {u.id} : {m}.{u.discriminator} doit être une constante d'énumération")
        for st in self.structs.values():
            for f in st.fields:
                self._check_spec(f"{st.id}.{f.name}", f.spec)
                if f.has_default:
                    self.check_value(f.spec, f.default, f"{st.id}.{f.name}(défaut)", allow_partial=True)
                if f.required and f.has_default:
                    raise ManifestError(f"{st.id}.{f.name} : un champ obligatoire n'a pas de défaut")
                for w in f.when:
                    disc = st.field(w["field"])
                    if disc.spec.kind != "enum":
                        raise ManifestError(f"{st.id}.{f.name} : condition sur un champ non énuméré")
                    en = self.enums[disc.spec.ref]
                    if "attr" in w:
                        en2 = self.enums[en.attrs[w["attr"]][5:]]
                        for x in w["in"]:
                            en2.by_wire(x)
                    else:
                        for x in w["in"]:
                            en.by_wire(x)
                if f.spec.kind == "switch":
                    disc = st.field(f.spec.switch_field)
                    en = self.enums[disc.spec.ref]
                    if sorted(f.spec.cases) != sorted(en.wires):
                        raise ManifestError(f"{st.id}.{f.name} : cas du switch ≠ valeurs de {en.id}")
        self.type_order()  # détecte les cycles

    def _check_constant(self, c: Constant) -> None:
        t = c.type
        if isinstance(t, dict):
            if not isinstance(c.value, list):
                raise ManifestError(f"constante {c.id} : tableau attendu")
            for v in c.value:
                self._check_scalar_const(c.id, t["array"], v)
        else:
            self._check_scalar_const(c.id, t, c.value)

    def _check_scalar_const(self, cid: str, t: str, v: Any) -> None:
        if t.startswith("enum:"):
            self.enums[t[5:]].by_wire(v)
        elif t == "string":
            if not isinstance(v, str):
                raise ManifestError(f"constante {cid} : chaîne attendue")
        elif t == "f64":
            if isinstance(v, bool) or not isinstance(v, (int, float)):
                raise ManifestError(f"constante {cid} : nombre attendu")
        elif t in INT_RANGES:
            lo, hi = INT_RANGES[t]
            if isinstance(v, bool) or not isinstance(v, int) or not lo <= v <= hi:
                raise ManifestError(f"constante {cid} : entier {t} attendu")
        else:
            raise ManifestError(f"constante {cid} : type {t} inconnu")

    def _check_spec(self, where: str, spec: Spec) -> None:
        if spec.kind == "enum":
            en = self.enums[spec.ref]
            if "const" in spec.constraints:
                en.by_wire(spec.constraints["const"])
        elif spec.kind == "struct":
            if spec.ref not in self.structs:
                raise ManifestError(f"{where} : type {spec.ref} inconnu")
        elif spec.kind == "union":
            if spec.ref not in self.unions:
                raise ManifestError(f"{where} : union {spec.ref} inconnue")
        elif spec.kind == "array":
            self._check_spec(where + "[]", spec.item)
        elif spec.kind == "switch":
            for c in spec.cases.values():
                self._check_spec(where, c)
        if "pattern" in spec.constraints:
            re.compile(spec.constraints["pattern"])

    def check_value(self, spec: Spec, value: Any, where: str, allow_partial: bool = False) -> None:
        """Vérifie qu'une valeur par défaut du manifeste respecte sa spécification."""
        if spec.kind == "array":
            if value == "$itemDefaults":
                return
            n = spec.array.get("length")
            if not isinstance(value, list) or (n is not None and len(value) != n):
                raise ManifestError(f"{where} : tableau de longueur {n} attendu")
            for v in value:
                self.check_value(spec.item, v, where + "[]", allow_partial)
        elif spec.kind == "struct":
            if not isinstance(value, dict) or (value and not allow_partial):
                raise ManifestError(f"{where} : défaut d'objet attendu {{}}")
        elif spec.kind == "f64":
            c = spec.constraints
            if isinstance(value, bool) or not isinstance(value, (int, float)):
                raise ManifestError(f"{where} : nombre attendu")
            if ("min" in c and value < c["min"]) or ("max" in c and value > c["max"]):
                raise ManifestError(f"{where} : défaut hors bornes")
            if ("exclusiveMin" in c and value <= c["exclusiveMin"]) or ("exclusiveMax" in c and value >= c["exclusiveMax"]):
                raise ManifestError(f"{where} : défaut hors bornes")
        elif spec.kind in INT_RANGES:
            c = spec.constraints
            if isinstance(value, bool) or not isinstance(value, int):
                raise ManifestError(f"{where} : entier attendu")
            if ("min" in c and value < c["min"]) or ("max" in c and value > c["max"]):
                raise ManifestError(f"{where} : défaut hors bornes")
        elif spec.kind == "bool":
            if not isinstance(value, bool):
                raise ManifestError(f"{where} : booléen attendu")

    # --------------------------------------------------------- dépendances
    def deps(self, spec: Spec) -> list[str]:
        if spec.kind == "struct":
            return [spec.ref]
        if spec.kind == "union":
            return list(self.unions[spec.ref].members) + [spec.ref]
        if spec.kind == "array":
            return self.deps(spec.item)
        if spec.kind == "switch":
            out: list[str] = []
            for c in spec.cases.values():
                for d in self.deps(c):
                    if d not in out:
                        out.append(d)
            return out
        return []

    def type_order(self) -> list[str]:
        """Ordre topologique déterministe des types et unions (dépendances d'abord)."""
        order: list[str] = []
        state: dict[str, int] = {}

        def visit(name: str) -> None:
            s = state.get(name, 0)
            if s == 2:
                return
            if s == 1:
                raise ManifestError(f"dépendance cyclique sur {name}")
            state[name] = 1
            if name in self.unions:
                for m in self.unions[name].members:
                    visit(m)
            else:
                for f in self.structs[name].fields:
                    for d in self.deps(f.spec):
                        visit(d)
            state[name] = 2
            order.append(name)

        for t in self.raw["types"]:
            visit(t["id"])
        for u in self.raw["unions"]:
            visit(u["id"])
        return order

    def reachable(self, roots: list[str]) -> list[str]:
        """Types, unions et énumérations atteignables depuis des racines, ordre déterministe."""
        seen: list[str] = []

        def visit_spec(spec: Spec) -> None:
            if spec.kind == "enum":
                if spec.ref not in seen:
                    seen.append(spec.ref)
            elif spec.kind in ("struct", "union"):
                visit(spec.ref)
            elif spec.kind == "array":
                visit_spec(spec.item)
            elif spec.kind == "switch":
                for c in spec.cases.values():
                    visit_spec(c)

        def visit(name: str) -> None:
            if name in seen:
                return
            seen.append(name)
            if name in self.unions:
                for m in self.unions[name].members:
                    visit(m)
            else:
                for f in self.structs[name].fields:
                    visit_spec(f.spec)

        for r in roots:
            visit(r)
        return seen


def load_manifest(path: Path = MANIFEST_PATH) -> Manifest:
    raw_bytes = path.read_bytes()
    from .strict_json import parse_strict  # import local : évite un cycle

    raw = parse_strict(raw_bytes, max_bytes=None, max_depth=None)
    return Manifest(raw, raw_bytes)


def canonical_json(value: Any) -> str:
    """Sérialisation canonique des fixtures et documents produits (LF final)."""
    return json.dumps(value, ensure_ascii=False, indent=2, allow_nan=False) + "\n"
