"""The Python and Java translators must produce identical SQL."""
import os
import subprocess
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, os.path.join(HERE, "..", "python"))
from skycell_adql import translate  # noqa: E402

CLASSES = os.environ.get("SKYCELL_JAVA_CLASSES", "/tmp/jb")


def main() -> int:
    bad = 0
    with open(os.path.join(HERE, "cases.txt")) as f:
        for line in f:
            q = line.rstrip("\n")
            if not q.strip():
                continue
            for dialect in ("skycell", "pgsphere"):
                py = translate(q, dialect)
                java = subprocess.run(
                    ["java", "-cp", CLASSES, "skycell.adql.SkycellAdql", q, dialect],
                    capture_output=True, text=True, check=True).stdout.strip()
                if py != java:
                    bad += 1
                    print(f"MISMATCH [{dialect}] {q}\n  python: {py}\n  java:   {java}")
    print("python and java agree on all cases" if not bad else f"{bad} mismatches")
    return 1 if bad else 0


if __name__ == "__main__":
    raise SystemExit(main())
