"""Cas négatifs : au moins 30, chacun refusé pour la raison attendue."""

from __future__ import annotations

import collections
import unittest

from contract_support import REPO, build_case, load_json, manifest, negative_cases
from xonar_contracts.validation import validate_bytes

REQUIRED_CATEGORIES = {"version", "size", "nonFinite", "mask", "array", "nyquist", "revision"}


class NegativeCases(unittest.TestCase):
    @classmethod
    def setUpClass(cls) -> None:
        cls.spec = negative_cases()
        cls.cases = cls.spec["cases"]

    def test_inventory(self) -> None:
        ids = [c["id"] for c in self.cases]
        self.assertEqual(len(ids), len(set(ids)), "identifiants de cas dupliqués")
        self.assertGreaterEqual(len(self.cases), 30)
        counts = collections.Counter(c["category"] for c in self.cases)
        for cat in REQUIRED_CATEGORIES:
            self.assertGreaterEqual(counts[cat], 3, f"catégorie {cat} insuffisamment couverte")

    def test_bases_are_valid_without_context(self) -> None:
        for key, rel in self.spec["bases"].items():
            with self.subTest(base=key):
                issues = validate_bytes((REPO / rel).read_bytes(), self.spec["targets"][key])
                self.assertEqual(issues, [], f"base {key} invalide : {[str(i) for i in issues]}")

    def test_each_case_rejected_with_expected_code(self) -> None:
        for case in self.cases:
            with self.subTest(case=case["id"]):
                data, target = build_case(self.spec, case)
                issues = validate_bytes(data, target, case.get("context"))
                codes = {i.code for i in issues}
                self.assertIn(case["expect"], codes,
                              f"{case['id']} ({case['doc']}) : obtenu {[str(i) for i in issues]}")
                if "expectPath" in case:
                    paths = {i.path for i in issues if i.code == case["expect"]}
                    self.assertIn(case["expectPath"], paths)

    def test_every_rule_has_a_negative_case(self) -> None:
        expected = {c["expect"] for c in self.cases}
        for rule in manifest().semantic_rules:
            rid = rule["id"]
            if rid.startswith("endpoint."):
                continue  # couvert par test_fixtures (validation d'endpoint distincte)
            with self.subTest(rule=rid):
                self.assertIn(rid, expected, f"règle {rid} sans cas négatif")

    def test_cases_file_is_strict_json(self) -> None:
        load_json(REPO / "tests/contracts/negative_cases.json")


if __name__ == "__main__":
    unittest.main()
