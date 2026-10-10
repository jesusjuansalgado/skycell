#!/usr/bin/env python3
"""Assemble ac/submission/: one self-contained .tex (the shared text and the
references inlined, figures beside it), the highlights file and the figure,
ready to upload to Elsevier's system, which does not resolve ../ paths."""
import os, re, shutil

HERE = os.path.dirname(os.path.abspath(__file__))
OUT = os.path.join(HERE, "submission")
os.makedirs(OUT, exist_ok=True)

def read(path):
    with open(path) as f:
        return f.read()

tex = read(os.path.join(HERE, "skycell-ac.tex"))
for name in ("content", "refs"):
    tex = tex.replace("\\input{../%s}" % name, read(os.path.join(HERE, "..", name + ".tex")))
tex = tex.replace("\\graphicspath{{../}}\n", "")
assert "\\input{" not in tex, "an \\input is left"
with open(os.path.join(OUT, "skycell-ac.tex"), "w") as f:
    f.write(tex)
for fig in sorted(set(re.findall(r"\\includegraphics(?:\[[^]]*\])?\{([^}]+)\}", tex))):
    shutil.copy(os.path.join(HERE, "..", fig), OUT)
shutil.copy(os.path.join(HERE, "highlights.txt"), OUT)
print("wrote", OUT)
