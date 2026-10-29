"""Validation d'un document v1 en ligne de commande.

Usage : python -I contracts/validate.py CIBLE FICHIER [--sample-rate HZ] [--normalize]
  CIBLE : profile-v1, control-v1, state-v1 ou <schéma>#<Définition>
          (ex. profile-v1#CapxDocument, control-v1#ChannelTestCommand).
  --sample-rate : fréquence de la connexion négociée (Nyquist, endpoint).
  --normalize   : affiche la forme normalisée d'un profil valide.
Code de retour : 0 valide, 1 invalide.
"""

from __future__ import annotations

import argparse
import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))

from xonar_contracts.model import canonical_json  # noqa: E402
from xonar_contracts.normalize import normalize  # noqa: E402
from xonar_contracts.semantic import check_endpoint_sample_rate  # noqa: E402
from xonar_contracts.strict_json import parse_strict  # noqa: E402
from xonar_contracts.validation import manifest, validate_bytes  # noqa: E402


def main(argv: list[str] | None = None) -> int:
    sys.stdout.reconfigure(encoding="utf-8", errors="replace")
    ap = argparse.ArgumentParser(description="Valide un document contre les contrats v1.")
    ap.add_argument("target")
    ap.add_argument("file", type=Path)
    ap.add_argument("--sample-rate", type=int)
    ap.add_argument("--normalize", action="store_true")
    args = ap.parse_args(argv)
    data = args.file.read_bytes()
    ctx = {"sampleRate": args.sample_rate} if args.sample_rate is not None else None
    issues = validate_bytes(data, args.target, ctx)
    for i in issues:
        print(i)
    print(f"invalide : {len(issues)} problème(s)" if issues else "valide")
    # Compatibilité d'endpoint : validation distincte de celle du DSP (C3).
    endpoint = check_endpoint_sample_rate(manifest(), args.sample_rate) if args.sample_rate is not None else []
    for i in endpoint:
        print(f"endpoint : {i}")
    if issues or endpoint:
        return 1
    if args.normalize and args.target in ("profile-v1", "profile-v1#ProfileDocument"):
        sys.stdout.write(canonical_json(normalize(manifest(), "ProfileDocument", parse_strict(data))))
    return 0


if __name__ == "__main__":
    sys.exit(main())
