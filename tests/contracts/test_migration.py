"""Migration v0 → v1 : non destructive, valeurs explicites conservées, refus explicites."""

from __future__ import annotations

import copy
import hashlib
import subprocess
import sys
import tempfile
import unittest
from pathlib import Path

from contract_support import CONTRACTS, REPO, load_json, manifest
from xonar_contracts.migration import migrate_v0
from xonar_contracts.validation import validate_value

V0_DIR = REPO / "configs" / "examples"
GOLDEN = REPO / "configs" / "examples" / "v1" / "migrations"
IDS = {
    "2.1-virtuel.json": ("3f1c0a6e-5b7d-4e2a-9c8b-1d2e3f4a5b01", "2.1", "preserve"),
    "ampli-2.1-downmix.json": ("3f1c0a6e-5b7d-4e2a-9c8b-1d2e3f4a5b02", "2.0", "preserve"),
    "passthrough-5.1.json": ("3f1c0a6e-5b7d-4e2a-9c8b-1d2e3f4a5b03", "5.1", "preserve"),
    "upmix-stereo-vers-5.1.json": ("3f1c0a6e-5b7d-4e2a-9c8b-1d2e3f4a5b04", "upmix", "autoDeclaredLayout"),
}
CLI = [sys.executable, "-I", str(CONTRACTS / "migrate_v0.py")]


def sha(path: Path) -> str:
    return hashlib.sha256(path.read_bytes()).hexdigest()


class Migration(unittest.TestCase):
    @classmethod
    def setUpClass(cls) -> None:
        cls.m = manifest()

    def run_cli(self, *args: str) -> subprocess.CompletedProcess:
        return subprocess.run(CLI + list(args), capture_output=True, text=True, encoding="utf-8")

    def test_four_examples_migrate_without_touching_originals(self) -> None:
        self.assertEqual(sorted(p.name for p in V0_DIR.glob("*.json")), sorted(IDS))
        with tempfile.TemporaryDirectory() as tmp:
            for name, (pid, mode, policy) in IDS.items():
                with self.subTest(example=name):
                    src = V0_DIR / name
                    before = sha(src)
                    out = Path(tmp) / name
                    r = self.run_cli(str(src), str(out), "--profile-id", pid)
                    self.assertEqual(r.returncode, 0, r.stdout + r.stderr)
                    self.assertEqual(sha(src), before, "l'original a été modifié")
                    self.assertEqual(out.read_bytes(), (GOLDEN / name).read_bytes())
                    doc = load_json(out)
                    self.assertEqual((doc["listeningMode"], doc["sourcePolicy"]), (mode, policy))
                    self.assertEqual(validate_value(doc, "profile-v1"), [])

    def test_explicit_values_preserved(self) -> None:
        for name, (pid, _, _) in IDS.items():
            with self.subTest(example=name):
                v0 = load_json(V0_DIR / name)
                v1 = migrate_v0(self.m, v0, pid).document
                self.assertEqual(v1["name"], v0["name"])
                self.assertEqual(v1["description"], v0["description"])
                for key in ("masterGainDb", "channelGainDb", "channelDelayMs", "channelMute", "bassManagement",
                            "limiter", "gainRampMs", "maxDelayMs", "downmix", "twoPointOne"):
                    if key in v0:
                        self.assertEqual(v1["output"][key], v0[key], key)
                if "upmix" in v0:
                    self.assertEqual(v1["routing"]["upmix"], v0["upmix"])
                for dropped in ("sampleRate", "inputMask", "outputMask", "mode", "$schema"):
                    self.assertNotIn(dropped, v1)

    def test_cli_never_overwrites(self) -> None:
        src = V0_DIR / "passthrough-5.1.json"
        before = sha(src)
        r = self.run_cli(str(src), str(src))
        self.assertEqual(r.returncode, 2)
        self.assertEqual(sha(src), before)
        with tempfile.TemporaryDirectory() as tmp:
            existing = Path(tmp) / "existe.json"
            existing.write_bytes(b"{}")
            r = self.run_cli(str(src), str(existing))
            self.assertEqual(r.returncode, 2)
            self.assertEqual(existing.read_bytes(), b"{}")

    def _migrate(self, mutate) -> object:
        v0 = load_json(V0_DIR / "upmix-stereo-vers-5.1.json")
        mutate(v0)
        return migrate_v0(self.m, v0, "3f1c0a6e-5b7d-4e2a-9c8b-1d2e3f4a5b09")

    def test_refusals_and_warnings(self) -> None:
        res = self._migrate(lambda d: d.update(mode="ChannelTest", outputMask=1551))
        self.assertEqual([i.code for i in res.errors], ["migration.channelTestNotPersistent"])
        res = self._migrate(lambda d: d.update(inputMask=0x43))
        self.assertIn("migration.maskUnknownBits", [i.code for i in res.errors])
        self.assertIsNone(res.document)
        res = self._migrate(lambda d: d.update(maxDelayMs=100))
        self.assertIn("migration.outOfV1Bounds", [i.code for i in res.errors])
        res = self._migrate(lambda d: d.update(sampleRate=88200))
        self.assertTrue(res.ok)
        self.assertIn("migration.sampleRateUnsupported", [i.code for i in res.warnings])
        res = self._migrate(lambda d: d.update(outputMask=63))
        self.assertTrue(res.ok)
        self.assertIn("migration.outputMaskIgnored", [i.code for i in res.warnings])
        res = self._migrate(lambda d: d.update(test={"channel": "FL"}))
        self.assertIn("migration.testGroupDropped", [i.code for i in res.warnings])
        res = self._migrate(lambda d: d.update(mode="Upmix2to6", inputMask=1551))
        self.assertIn("migration.inputMaskIgnored", [i.code for i in res.warnings])
        res = self._migrate(lambda d: d.update(sampleRate=12345))
        self.assertIn("migration.invalidV0", [i.code for i in res.errors])

    def test_v1_document_is_not_migrated(self) -> None:
        v1 = load_json(GOLDEN / "passthrough-5.1.json")
        res = migrate_v0(self.m, copy.deepcopy(v1), "3f1c0a6e-5b7d-4e2a-9c8b-1d2e3f4a5b0a")
        self.assertEqual([i.code for i in res.errors], ["migration.notV0"])


if __name__ == "__main__":
    unittest.main()
