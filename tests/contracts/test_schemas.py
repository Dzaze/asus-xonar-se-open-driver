"""Schémas v1 : forme Draft 2020-12, objets fermés, v0 conservé pour l'import."""

from __future__ import annotations

import unittest
from typing import Any, Iterator

from contract_support import REPO, load_json, manifest
from xonar_contracts.schema_check import Validator

DRAFT = "https://json-schema.org/draft/2020-12/schema"


def subschemas(s: Any, path: str = "#") -> Iterator[tuple[str, dict]]:
    if not isinstance(s, dict):
        return
    yield path, s
    for k in ("items", "not", "if", "then", "else"):
        if k in s:
            yield from subschemas(s[k], f"{path}/{k}")
    for k in ("properties", "$defs"):
        for name, sub in s.get(k, {}).items():
            yield from subschemas(sub, f"{path}/{k}/{name}")
    for k in ("allOf", "anyOf", "oneOf"):
        for i, sub in enumerate(s.get(k, [])):
            yield from subschemas(sub, f"{path}/{k}/{i}")


class Schemas(unittest.TestCase):
    @classmethod
    def setUpClass(cls) -> None:
        cls.m = manifest()
        cls.schemas = {k: load_json(REPO / info["file"]) for k, info in cls.m.schemas.items()}

    def test_three_v1_schema_files(self) -> None:
        self.assertEqual(sorted(info["file"] for info in self.m.schemas.values()), [
            "configs/profiles/control-v1.schema.json",
            "configs/profiles/profile-v1.schema.json",
            "configs/profiles/state-v1.schema.json",
        ])

    def test_draft_2020_12_and_supported_keywords(self) -> None:
        for key, s in self.schemas.items():
            with self.subTest(schema=key):
                self.assertEqual(s["$schema"], DRAFT)
                self.assertEqual(s["$id"], self.m.schemas[key]["id"])
                Validator(s)  # lève SchemaUnsupported sur tout mot-clé inconnu

    def test_every_object_definition_is_closed(self) -> None:
        for key, s in self.schemas.items():
            for name, d in s["$defs"].items():
                if d.get("type") == "object":
                    with self.subTest(schema=key, definition=name):
                        self.assertIs(d.get("additionalProperties"), False)
            for path, sub in subschemas(s):
                if "properties" in sub and sub.get("type") == "object":
                    with self.subTest(schema=key, path=path):
                        self.assertIs(sub.get("additionalProperties"), False)

    def test_refs_resolve(self) -> None:
        for key, s in self.schemas.items():
            v = Validator(s)
            for path, sub in subschemas(s):
                if "$ref" in sub:
                    with self.subTest(schema=key, ref=sub["$ref"]):
                        v.resolve(sub["$ref"])

    def test_entries_exposed(self) -> None:
        for key, info in self.m.schemas.items():
            for entry in info["entries"]:
                self.assertIn(entry, self.schemas[key]["$defs"])

    def test_v0_schema_kept_for_import(self) -> None:
        v0_path = REPO / self.m.raw["v0"]["schemaFile"]
        v0 = load_json(v0_path)
        self.assertNotIn("schemaVersion", v0["properties"])
        self.assertIn(88200, v0["properties"]["sampleRate"]["enum"])  # v0 inchangé
        v = Validator(v0)
        examples = sorted((REPO / self.m.raw["v0"]["examplesDir"]).glob("*.json"))
        self.assertEqual(len(examples), 4)
        for ex in examples:
            with self.subTest(example=ex.name):
                self.assertEqual(v.validate(load_json(ex)), [])

    def test_validator_strictness(self) -> None:
        v = Validator({"type": "integer"})
        self.assertEqual(v.validate(1), [])
        self.assertTrue(v.validate(1.0), "1.0 doit être refusé comme entier")
        self.assertTrue(v.validate(True), "un booléen n'est pas un entier")
        self.assertTrue(Validator({"const": 1}).validate(True))
        self.assertTrue(Validator({"enum": [0]}).validate(False))


if __name__ == "__main__":
    unittest.main()
