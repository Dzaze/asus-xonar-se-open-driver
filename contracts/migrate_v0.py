"""Migration d'un profil v0 vers v1, sans jamais écraser l'original.

Usage : python -I contracts/migrate_v0.py ENTREE.json SORTIE.json [--profile-id UUID] [--report RAPPORT.json]

- SORTIE ne doit pas exister et doit différer de ENTREE (aucun écrasement) ;
- écriture via un fichier temporaire puis remplacement atomique ;
- code de retour : 0 succès (avertissements possibles), 1 refus, 2 usage.
"""

from __future__ import annotations

import argparse
import json
import os
import sys
import uuid
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))

from xonar_contracts.migration import migrate_v0  # noqa: E402
from xonar_contracts.model import canonical_json, load_manifest  # noqa: E402
from xonar_contracts.strict_json import JsonError, parse_strict  # noqa: E402


def main(argv: list[str] | None = None) -> int:
    sys.stdout.reconfigure(encoding="utf-8", errors="replace")
    ap = argparse.ArgumentParser(description="Migre un profil v0 vers v1 sans écraser l'original.")
    ap.add_argument("input", type=Path)
    ap.add_argument("output", type=Path)
    ap.add_argument("--profile-id", help="UUID à affecter (défaut : nouvel UUID v4)")
    ap.add_argument("--report", type=Path, help="rapport JSON des diagnostics")
    args = ap.parse_args(argv)
    m = load_manifest()
    src, dst = args.input.resolve(), args.output.resolve()
    if src == dst:
        print("refus : la sortie doit différer de l'entrée (l'original n'est jamais écrasé)")
        return 2
    if dst.exists():
        print(f"refus : {dst} existe déjà")
        return 2
    profile_id = args.profile_id or str(uuid.uuid4())
    try:
        doc = parse_strict(src.read_bytes(), max_bytes=m.const("MaxDocumentBytes"), max_depth=m.const("MaxDocumentDepth"))
    except JsonError as exc:
        print(f"refus : {exc}")
        return 1
    res = migrate_v0(m, doc, profile_id)
    for level, items in (("erreur", res.errors), ("avertissement", res.warnings), ("info", res.infos)):
        for i in items:
            print(f"{level} : {i}")
    if args.report:
        report = {level: [{"code": i.code, "path": i.path, "message": i.message} for i in items]
                  for level, items in (("errors", res.errors), ("warnings", res.warnings), ("infos", res.infos))}
        args.report.write_text(json.dumps(report, ensure_ascii=False, indent=2) + "\n", encoding="utf-8", newline="\n")
    if not res.ok:
        print("migration refusée : aucun fichier écrit")
        return 1
    tmp = dst.with_name(dst.name + ".tmp")
    tmp.write_bytes(canonical_json(res.document).encode("utf-8"))
    os.replace(tmp, dst)
    print(f"écrit : {dst}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
