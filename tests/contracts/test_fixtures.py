"""Fixtures positives : modes, politiques, exemple §1.4, normalisation, bornes."""

from __future__ import annotations

import copy
import itertools
import json
import re
import unittest

from contract_support import REPO, canonical_json, fixture_targets, load_json, manifest
from xonar_contracts.normalize import normalize
from xonar_contracts.semantic import check_endpoint_sample_rate
from xonar_contracts.strict_json import JsonError, parse_strict
from xonar_contracts.validation import matched_root, validate_bytes, validate_value

V1 = REPO / "configs" / "examples" / "v1"
SECTION_14 = V1 / "profiles" / "exemple-section-1.4.json"
SECTION_14_NORMALIZED = V1 / "normalized" / "exemple-section-1.4.json"


def backlog_example() -> dict:
    text = (REPO / "docs" / "backlog.md").read_text(encoding="utf-8")
    start = text.index("Exemple de forme, à matérialiser en E0-02")
    block = re.search(r"```json\n(.*?)\n```", text[start:], re.S).group(1)
    return json.loads(block)


class PositiveFixtures(unittest.TestCase):
    @classmethod
    def setUpClass(cls) -> None:
        cls.m = manifest()

    def test_all_fixtures_valid(self) -> None:
        found = fixture_targets()
        self.assertGreaterEqual(len(found), 30)
        for fx in self.m.fixtures:
            self.assertTrue(list((REPO / fx["dir"]).glob("*.json")), f"répertoire vide : {fx['dir']}")
        for path, target in found:
            with self.subTest(fixture=str(path.relative_to(REPO))):
                issues = validate_bytes(path.read_bytes(), target)
                self.assertEqual(issues, [], [str(i) for i in issues])

    def test_fixtures_are_canonical_utf8_lf(self) -> None:
        for path, _ in fixture_targets():
            data = path.read_bytes()
            with self.subTest(fixture=path.name):
                self.assertNotIn(b"\r", data)
                self.assertFalse(data.startswith(b"\xef\xbb\xbf"))
                if path != SECTION_14:  # copie verbatim du backlog, mise en forme d'origine
                    self.assertEqual(data, canonical_json(load_json(path)).encode("utf-8"))

    def test_four_modes_and_three_policies(self) -> None:
        modes = self.m.enums["ListeningMode"].wires
        policies = self.m.enums["SourcePolicy"].wires
        self.assertEqual(len(modes), 4)
        self.assertEqual(len(policies), 3)
        seen_modes, seen_policies = set(), set()
        for path in sorted((V1 / "profiles").glob("*.json")):
            doc = load_json(path)
            seen_modes.add(doc["listeningMode"])
            seen_policies.add(doc["sourcePolicy"])
        self.assertEqual(seen_modes, set(modes), "fixtures versionnées : modes manquants")
        self.assertEqual(seen_policies, set(policies), "fixtures versionnées : politiques manquantes")
        base = load_json(SECTION_14)
        for mode, policy in itertools.product(modes, policies):
            doc = copy.deepcopy(base)
            doc["listeningMode"], doc["sourcePolicy"] = mode, policy
            for rate in self.m.const("XonarRenderSampleRates"):
                with self.subTest(mode=mode, policy=policy, rate=rate):
                    self.assertEqual(validate_value(doc, "profile-v1", {"sampleRate": rate}), [])
                    self.assertEqual(validate_value(normalize(self.m, "ProfileDocument", doc), "profile-v1",
                                                    {"sampleRate": rate}), [])

    def test_section_14_example_accepted_after_normalization(self) -> None:
        raw = load_json(SECTION_14)
        self.assertEqual(raw, backlog_example(), "la fixture doit être l'exemple exact du §1.4")
        self.assertEqual(validate_value(raw, "profile-v1"), [])
        norm = normalize(self.m, "ProfileDocument", raw)
        self.assertEqual(validate_value(norm, "profile-v1"), [])
        self.assertEqual(norm, load_json(SECTION_14_NORMALIZED))
        self.assertEqual(normalize(self.m, "ProfileDocument", norm), norm, "normalisation non idempotente")
        self.assertEqual(len(norm["output"]["eq"]), 8)
        self.assertTrue(all(len(row) == 10 for row in norm["output"]["eq"]))
        for key in ("downmix", "twoPointOne", "gainRampMs", "maxDelayMs"):
            self.assertIn(key, norm["output"])
        # Les valeurs explicites de l'exemple sont conservées.
        self.assertEqual(norm["routing"], raw["routing"])
        self.assertEqual(norm["output"]["limiter"], raw["output"]["limiter"])
        # Seule la forme normalisée est publiable dans CAPX.
        capx = {"contractMajor": 1, "contractMinor": 0, "epoch": 1, "generation": 1}
        self.assertEqual(validate_value(dict(capx, payload=norm), "profile-v1#CapxDocument"), [])
        codes = {i.code for i in validate_value(dict(capx, payload=raw), "profile-v1#CapxDocument")}
        self.assertEqual(codes, {"semantic.capxPayloadNotNormalized"})

    def test_control_fixtures_cover_commands_and_results(self) -> None:
        commands, kinds, roots = set(), set(), set()
        for path in (V1 / "control").glob("*.json"):
            doc = load_json(path)
            roots.add(matched_root(doc, "control-v1"))
            if "command" in doc:
                commands.add(doc["command"])
            if isinstance(doc.get("result"), dict):
                kinds.add(doc["result"]["kind"])
        self.assertEqual(commands, set(self.m.enums["Command"].wires))
        self.assertEqual(kinds, set(self.m.enums["ResultKind"].wires))
        self.assertEqual(roots, {"ControlRequest", "ControlResponse", "StateNotification"})

    def test_size_and_depth_boundaries(self) -> None:
        data = SECTION_14.read_bytes()
        exact = data + b" " * (self.m.const("MaxDocumentBytes") - len(data))
        self.assertEqual(len(exact), 131072)
        self.assertEqual(validate_bytes(exact, "profile-v1"), [])
        parse_strict(b"[" * 16 + b"1" + b"]" * 16)
        with self.assertRaises(JsonError):
            parse_strict(b"[" * 17 + b"1" + b"]" * 17)
        # Les crochets dans une chaîne ne comptent pas dans la profondeur.
        parse_strict(json.dumps({"x": "[" * 40}).encode())

    def test_uint64_extremes(self) -> None:
        doc = load_json(SECTION_14)
        doc["revision"] = 2**64 - 1
        self.assertEqual(validate_value(doc, "profile-v1"), [])

    def test_nyquist_ok_at_every_xonar_rate(self) -> None:
        data = (V1 / "profiles" / "eq-80-bandes.json").read_bytes()
        for rate in self.m.const("XonarRenderSampleRates"):
            with self.subTest(rate=rate):
                self.assertEqual(validate_bytes(data, "profile-v1", {"sampleRate": rate}), [])

    def test_endpoint_validation_is_distinct_from_dsp(self) -> None:
        data = SECTION_14.read_bytes()
        # 88,2 kHz est une fréquence DSP portable valide…
        self.assertEqual(validate_bytes(data, "profile-v1", {"sampleRate": 88200}), [])
        # …mais jamais annoncée par le rendu Xonar actuel.
        self.assertEqual([i.code for i in check_endpoint_sample_rate(self.m, 88200)], ["endpoint.sampleRateUnsupported"])
        self.assertEqual([i.code for i in check_endpoint_sample_rate(self.m, 176400)], ["endpoint.sampleRateUnsupported"])
        for rate in self.m.const("XonarRenderSampleRates"):
            self.assertEqual(check_endpoint_sample_rate(self.m, rate), [])


if __name__ == "__main__":
    unittest.main()
