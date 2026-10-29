"""Générateur : reproductibilité octet pour octet, artefacts à jour, GUID uniquement du manifeste."""

from __future__ import annotations

import filecmp
import re
import subprocess
import sys
import tempfile
import unittest
from pathlib import Path

from contract_support import CONTRACTS, REPO, manifest

GENERATE = [sys.executable, "-I", str(CONTRACTS / "generate.py")]
GUID_RE = re.compile(r"[0-9A-Fa-f]{8}-[0-9A-Fa-f]{4}-[0-9A-Fa-f]{4}-[0-9A-Fa-f]{4}-[0-9A-Fa-f]{12}")


def tree(root: Path) -> dict[str, bytes]:
    return {p.relative_to(root).as_posix(): p.read_bytes() for p in sorted(root.rglob("*")) if p.is_file()}


class Generator(unittest.TestCase):
    @classmethod
    def setUpClass(cls) -> None:
        cls.m = manifest()
        cls.outputs = sorted([info["file"] for info in cls.m.schemas.values()] + list(cls.m.outputs.values()))

    def test_two_generations_byte_identical(self) -> None:
        with tempfile.TemporaryDirectory() as a, tempfile.TemporaryDirectory() as b:
            for d in (a, b):
                r = subprocess.run(GENERATE + ["--out-root", d], capture_output=True, text=True, encoding="utf-8")
                self.assertEqual(r.returncode, 0, r.stdout + r.stderr)
            ta, tb = tree(Path(a)), tree(Path(b))
            self.assertEqual(sorted(ta), self.outputs)
            self.assertEqual(ta, tb, "deux générations successives diffèrent")
            for rel in self.outputs:
                self.assertTrue(filecmp.cmp(Path(a) / rel, REPO / rel, shallow=False), f"{rel} périmé dans le dépôt")

    def test_check_mode(self) -> None:
        r = subprocess.run(GENERATE + ["--check"], capture_output=True, text=True, encoding="utf-8")
        self.assertEqual(r.returncode, 0, r.stdout)

    def test_outputs_utf8_lf_no_bom_and_tagged(self) -> None:
        for rel in self.outputs:
            data = (REPO / rel).read_bytes()
            with self.subTest(file=rel):
                self.assertFalse(data.startswith(b"\xef\xbb\xbf"))
                self.assertNotIn(b"\r", data)
                self.assertTrue(data.endswith(b"\n"))
                self.assertIn(self.m.sha256.encode(), data)
                data.decode("utf-8")

    def test_guids_come_only_from_manifest(self) -> None:
        values = {g["value"].upper() for g in self.m.guids.values()}
        header = (REPO / self.m.outputs["cppHeader"]).read_text(encoding="utf-8")
        csharp = (REPO / self.m.outputs["csharp"]).read_text(encoding="utf-8")
        for g in values:
            self.assertIn("{" + g + "}", header)
            self.assertIn(f'new Guid("{g}")', csharp)
            d1, d2 = g.split("-")[:2]
            self.assertIn(f"0x{d1}u, 0x{d2}u", header)
        self.assertEqual({x.upper() for x in GUID_RE.findall(header)}, values)
        self.assertEqual({x.upper() for x in GUID_RE.findall(csharp)}, values)
        # Aucun code du générateur n'attribue de GUID.
        for py in [CONTRACTS / "generate.py", *sorted((CONTRACTS / "xonar_contracts").glob("gen_*.py")),
                   CONTRACTS / "xonar_contracts" / "model.py"]:
            text = py.read_text(encoding="utf-8")
            with self.subTest(source=py.name):
                self.assertNotRegex(text, r"\buuid\d?\(|import uuid")
                self.assertEqual(GUID_RE.findall(text), [])
        # Les schémas ne portent aucun GUID CAPX/CLSID.
        for info in self.m.schemas.values():
            text = (REPO / info["file"]).read_text(encoding="utf-8")
            self.assertFalse(values & {x.upper() for x in GUID_RE.findall(text)})

    def test_no_enum_hand_duplicated_in_tests(self) -> None:
        # Les tests C++/C# s'appuient sur les tables générées, pas sur des copies.
        for src in (REPO / "tests/contracts/cpp/conformance_main.cpp", REPO / "tests/contracts/csharp/ContractsSmoke.cs"):
            text = src.read_text(encoding="utf-8")
            with self.subTest(source=src.name):
                self.assertNotIn("enum class", text)
                self.assertNotRegex(text, r"\benum\s+\w+\s*[:{]")


if __name__ == "__main__":
    unittest.main()
