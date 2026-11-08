#!/usr/bin/env python3
"""Run the tutorials' SQL and check what they promise.

Each tutorial runs in a fresh database named skycell_tutorial (dropped and
created again: do not point this at a server where that name matters).  Every
```sql block is run in order, one psql session per block, stopping at the
first error.  A line "-- expect: TEXT" in a block requires TEXT in that block's
output (an EXPLAIN plan node, say); "-- expect-not: TEXT" forbids it.  A block
preceded by the line <!-- check: skip --> is not run.

    PGHOST=/tmp PGPORT=5432 PGUSER=postgres ./check.py [tutorial.md ...]

Exit status 0 when every tutorial passes.
"""
import os, re, subprocess, sys, glob

DB = "skycell_tutorial"

def blocks(path):
    text = open(path).read()
    out, skip = [], False
    for m in re.finditer(r"(<!-- check: skip -->\s*)?```sql\n(.*?)```", text, re.S):
        if not m.group(1):
            out.append(m.group(2))
    return out

def psql(sql, db):
    return subprocess.run(["psql", "-X", "-v", "ON_ERROR_STOP=1", "-d", db],
                          input=sql, capture_output=True, text=True)

def run(path):
    for cmd in (["dropdb", "--if-exists", DB], ["createdb", DB]):
        subprocess.run(cmd, check=True, capture_output=True)
    ok = True
    for i, sql in enumerate(blocks(path), 1):
        r = psql(sql, DB)
        out = r.stdout + r.stderr
        if r.returncode != 0:
            print(f"{path}: block {i} failed:\n{sql}\n{out}")
            return False
        for kind, want in re.findall(r"--\s*(expect|expect-not):\s*(.+)", sql):
            want = want.strip()
            if (kind == "expect") != (want in out):
                print(f"{path}: block {i}: {kind} '{want}' not met:\n{out}")
                ok = False
    print(f"{path}: {'ok' if ok else 'FAILED'} ({len(blocks(path))} blocks)")
    return ok

if __name__ == "__main__":
    here = os.path.dirname(os.path.abspath(__file__))
    files = sys.argv[1:] or sorted(glob.glob(os.path.join(here, "[0-9]*.md")))
    sys.exit(0 if all([run(f) for f in files]) else 1)
