"""Générateur des artefacts de contrats v1 depuis contracts/manifest.json.

Usage (toujours avec python -I) :
  python -I contracts/generate.py                 # écrit dans le dépôt
  python -I contracts/generate.py --check         # échoue si un fichier diffère
  python -I contracts/generate.py --out-root DIR  # écrit sous DIR (test de reproductibilité)

Produit, octet pour octet de façon déterministe (UTF-8 sans BOM, LF) :
  configs/profiles/{profile,control,state}-v1.schema.json
  contracts/generated/cpp/include/xonar/contracts/v1/contracts.hpp
  contracts/generated/csharp/Xonar.Contracts.V1.g.cs
  tests/contracts/cpp/dsp_conformance.g.inc
Le générateur n'attribue aucun GUID : il recopie ceux du manifeste.
"""

from __future__ import annotations

import argparse
import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))

from xonar_contracts.gen_cpp import generate_dsp_conformance, generate_header  # noqa: E402
from xonar_contracts.gen_csharp import generate_csharp  # noqa: E402
from xonar_contracts.gen_schema import build_schemas  # noqa: E402
from xonar_contracts.model import REPO_ROOT, canonical_json, load_manifest  # noqa: E402


def build_outputs() -> dict[str, bytes]:
    m = load_manifest()
    out: dict[str, str] = {}
    for key, schema in build_schemas(m).items():
        out[m.schemas[key]["file"]] = canonical_json(schema)
    out[m.outputs["cppHeader"]] = generate_header(m)
    out[m.outputs["csharp"]] = generate_csharp(m)
    out[m.outputs["cppDspConformance"]] = generate_dsp_conformance(m)
    return {path: text.encode("utf-8") for path, text in sorted(out.items())}


def main(argv: list[str] | None = None) -> int:
    ap = argparse.ArgumentParser(description="Génère les schémas et DTO v1 depuis le manifeste.")
    ap.add_argument("--check", action="store_true", help="compare au dépôt sans écrire")
    ap.add_argument("--out-root", type=Path, default=REPO_ROOT, help="racine de sortie (défaut : dépôt)")
    args = ap.parse_args(argv)
    sys.stdout.reconfigure(encoding="utf-8", errors="replace")
    outputs = build_outputs()
    if args.check:
        stale = []
        for rel, data in outputs.items():
            path = REPO_ROOT / rel
            current = path.read_bytes().replace(b"\r\n", b"\n") if path.exists() else None
            if current != data:
                stale.append(rel)
        for rel in stale:
            print(f"périmé : {rel}")
        if stale:
            print("Relancer : python -I contracts/generate.py")
            return 1
        print(f"{len(outputs)} fichiers générés à jour.")
        return 0
    root = args.out_root.resolve()
    for rel, data in outputs.items():
        path = root / rel
        path.parent.mkdir(parents=True, exist_ok=True)
        tmp = path.with_name(path.name + ".tmp")
        tmp.write_bytes(data)
        tmp.replace(path)
        print(f"écrit : {rel} ({len(data)} octets)")
    return 0


if __name__ == "__main__":
    sys.exit(main())
