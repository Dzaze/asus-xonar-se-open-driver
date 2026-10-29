"""Lance tous les tests Python des contrats v1.

Usage : python -I tests/contracts/run_contract_tests.py [-v]
Bibliothèque standard seule (unittest) ; aucun paquet pip, aucun réseau.
"""

from __future__ import annotations

import sys
import unittest
from pathlib import Path

HERE = Path(__file__).resolve().parent


def main() -> int:
    sys.stdout.reconfigure(encoding="utf-8", errors="replace")
    sys.stderr.reconfigure(encoding="utf-8", errors="replace")
    sys.path.insert(0, str(HERE))
    suite = unittest.defaultTestLoader.discover(str(HERE), pattern="test_*.py", top_level_dir=str(HERE))
    verbosity = 2 if "-v" in sys.argv[1:] else 1
    result = unittest.TextTestRunner(verbosity=verbosity, stream=sys.stdout).run(suite)
    return 0 if result.wasSuccessful() else 1


if __name__ == "__main__":
    sys.exit(main())
