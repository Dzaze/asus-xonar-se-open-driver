"""Cohérence du manifeste et fidélité de la transcription du backlog §1."""

from __future__ import annotations

import re
import unittest

from contract_support import REPO, manifest

BACKLOG = REPO / "docs" / "backlog.md"


def backlog_text() -> str:
    return BACKLOG.read_text(encoding="utf-8")


class ManifestCoherence(unittest.TestCase):
    @classmethod
    def setUpClass(cls) -> None:
        cls.m = manifest()  # lève ManifestError si incohérent

    def test_contracts_c1_to_c8_version_1_0(self) -> None:
        self.assertEqual([c["id"] for c in self.m.contracts], [f"C{i}" for i in range(1, 9)])
        for c in self.m.contracts:
            self.assertEqual((c["major"], c["minor"]), (1, 0), c["id"])
            self.assertIn(f"### {c['section'][1:]}. {c['id']} ", backlog_text().replace("§", ""))

    def test_guids_assigned_once_by_e0_02(self) -> None:
        values = [g["value"] for g in self.m.guids.values()]
        self.assertEqual(len(values), 5)
        self.assertEqual(len(set(values)), len(values))
        for g in self.m.guids.values():
            self.assertEqual(g["assignedBy"], "E0-02")
            self.assertRegex(g["value"], r"^[0-9A-F]{8}-[0-9A-F]{4}-4[0-9A-F]{3}-[89AB][0-9A-F]{3}-[0-9A-F]{12}$")

    def test_enums_match_backlog_text(self) -> None:
        text = backlog_text()
        modes = re.search(r"`listeningMode ∈ \{([^}]*)\}`", text).group(1)
        policies = re.search(r"`sourcePolicy ∈ \{([^}]*)\}`", text).group(1)
        self.assertEqual(self.m.enums["ListeningMode"].wires, re.findall(r'"([^"]+)"', modes))
        self.assertEqual(self.m.enums["SourcePolicy"].wires, re.findall(r'"([^"]+)"', policies))
        commands = re.search(r"Commandes : ([^.]*)\.", text).group(1)
        errors = re.search(r"Erreurs stables : ([^.]*)\.", text).group(1)
        self.assertEqual(self.m.enums["Command"].wires, re.findall(r"`(\w+)`", commands))
        self.assertEqual(self.m.enums["ErrorCode"].wires, re.findall(r"`(\w+)`", errors))
        self.assertIn('`endpointRole="speakers"`', text)
        self.assertEqual(self.m.enums["EndpointRole"].wires, ["speakers"])

    def test_c1_additions_match_backlog(self) -> None:
        text = backlog_text()
        self.assertIn("enum class PublishResult : std::uint8_t { Accepted, Busy, Invalid };", text)
        self.assertIn("enum class ProcessingStage : std::uint8_t { RoutingOnly, Output };", text)
        self.assertEqual([v.id for v in self.m.enums["PublishResult"].values], ["Accepted", "Busy", "Invalid"])
        self.assertEqual([v.id for v in self.m.enums["ProcessingStage"].values], ["RoutingOnly", "Output"])

    def test_c3_required_fields_match_backlog(self) -> None:
        text = backlog_text()
        listed = re.search(r"champs obligatoires ([^;]*?) ; Exemple", text) or re.search(r"champs obligatoires (.*?)\. Exemple", text)
        names = re.findall(r"`(\w+)`", listed.group(1))
        required = [f.name for f in self.m.structs["ProfileDocument"].fields if f.required]
        self.assertEqual(required, names)

    def test_c3_c4_limits(self) -> None:
        m = self.m
        self.assertEqual(m.const("MaxDocumentBytes"), 128 * 1024)
        self.assertEqual(m.const("MaxDocumentDepth"), 16)
        self.assertEqual(m.const("MaxFrameBytes"), 128 * 1024)
        self.assertEqual(m.const("CommandTimeoutMs"), 2000)
        self.assertEqual(m.const("ReplayCacheEntries"), 256)
        self.assertEqual(m.const("PipeNamePrefix"), "\\\\.\\pipe\\XonarSE.Control.v1.")
        self.assertIn("`\\\\.\\pipe\\XonarSE.Control.v1.<sessionId>`", backlog_text())
        self.assertNotIn(88200, m.const("XonarRenderSampleRates"))
        self.assertNotIn(176400, m.const("XonarRenderSampleRates"))

    def test_nyquist_fields_defaults_below_portable_nyquist(self) -> None:
        nyq = self.m.const("DspSampleRateMin") / 2
        for st in self.m.structs.values():
            for f in st.fields:
                if f.nyquist and f.has_default:
                    self.assertLess(f.default, nyq, f"{st.id}.{f.name}")

    def test_semantic_rule_ids_unique(self) -> None:
        ids = [r["id"] for r in self.m.semantic_rules]
        self.assertEqual(len(ids), len(set(ids)))

    def test_no_channel_test_in_profile(self) -> None:
        names = {f.name for f in self.m.structs["OutputSettings"].fields} | \
                {f.name for f in self.m.structs["ProfileDocument"].fields}
        for forbidden in ("test", "sampleRate", "inputMask", "outputMask", "mode", "$schema"):
            self.assertNotIn(forbidden, names)


if __name__ == "__main__":
    unittest.main()
