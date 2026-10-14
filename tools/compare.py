"""Diff tree-sitter parses against the official shrubbery reader.

Usage: uv run --no-project python tools/compare.py FILE ...

Needs the devenv Racket (run inside `devenv shell`, or with PLTADDONDIR set).
"""

import os
import subprocess
import sys
import xml.etree.ElementTree as ET

sys.path.insert(0, os.path.dirname(__file__))
from ts2shrub import conv  # noqa: E402

HERE = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))


def reference(files):
    out = subprocess.run(
        ["racket", os.path.join(HERE, "tools/ref.rkt"), *files],
        capture_output=True,
        text=True,
        check=True,
    ).stdout
    return dict(line.split("\t", 1) for line in out.splitlines())


def treesitter(files):
    out = subprocess.run(
        ["tree-sitter", "parse", "-x", *files],
        capture_output=True,
        text=True,
        cwd=HERE,
    ).stdout
    # the CLI appends a summary after the XML for files with errors
    root = ET.fromstring(out[: out.index("</sources>") + len("</sources>")])
    return {
        src.get("name"): conv(next(iter(src))) for src in root.iter("source")
    }


def first_diff(a, b):
    i = next((i for i, (x, y) in enumerate(zip(a, b)) if x != y), min(len(a), len(b)))
    lo = max(0, i - 120)
    return a[lo : i + 80], b[lo : i + 80]


def main():
    files = [os.path.abspath(f) for f in sys.argv[1:]]
    ref = reference(files)
    ts = treesitter(files)
    ok = skipped = 0
    for f in files:
        r, t = ref[f], ts.get(f, "?missing")
        if r.startswith("ERROR"):
            skipped += 1
            continue
        if r == t:
            ok += 1
            continue
        a, b = first_diff(r, t)
        print(f"MISMATCH {os.path.relpath(f)}\n  ref: …{a}\n  ts:  …{b}\n")
    total = len(files) - skipped
    print(f"{ok}/{total} match ({skipped} rejected by the reference reader)")


if __name__ == "__main__":
    main()
