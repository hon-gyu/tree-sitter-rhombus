"""Convert `tree-sitter parse -x` output into the normalized form of tools/ref.rkt.

Usage: tree-sitter parse -x FILE | uv run tools/ts2shrub.py
"""

import re
import sys
import xml.etree.ElementTree as ET

SEQ = {"parens", "brackets", "braces", "quotes", "block"}
SKIP = {"comment", "group_comment", "alternative_comment", "lang_line"}
ATOMS = {
    "number": "num",
    "string": "str",
    "byte_string": "bytes",
    "boolean": "bool",
    "void": "void",
}


def kids(e):
    return [c for c in e if c.tag not in SKIP]


def conv(e):
    tag = e.tag
    if tag == "source_file":
        return "(" + " ".join(["multi", *map(conv, kids(e))]) + ")"
    if tag == "group":
        ks = kids(e)
        parts = []
        for i, c in enumerate(ks):
            # the reference drops an empty `:` (but not `:«»`) before alternatives
            if (
                c.tag == "block"
                and not kids(c)
                and "«" not in "".join(c.itertext())
                and i + 1 < len(ks)
                and ks[i + 1].tag == "alternatives"
            ):
                continue
            parts.append(conv(c))
        return "(" + " ".join(["group", *parts]) + ")"
    if tag in SEQ:
        return "(" + " ".join([tag, *map(conv, kids(e))]) + ")"
    if tag == "splice":
        # spliced groups are inlined by the caller
        return " ".join(map(conv, kids(e)))
    if tag == "alternatives":
        return "(" + " ".join(["alts", *map(conv, kids(e))]) + ")"
    if tag == "alternative":
        return "(" + " ".join(["block", *map(conv, kids(e))]) + ")"
    if tag == "identifier":
        return e.text.strip()
    if tag == "keyword":
        return e.text.strip()
    if tag == "operator":
        return f"(op {e.text.strip()})"
    if tag == "sexp_escape":
        # `#{name}` is how non-shrubbery identifiers are written
        text = e.text.strip()
        prefix = "~" if text.startswith("~") else ""
        inner = text[text.index("{") + 1 : -1]
        # match how ref.rkt prints the Racket value
        if inner.startswith("#\\"):
            return "char"
        if re.fullmatch(r"[-+0-9.e/i#xob]+", inner) and re.search(r"\d", inner):
            return "num"
        if inner.startswith("#") and not inner.startswith("#%"):
            return "?" + inner
        return prefix + inner.strip("|").replace("\\", "")
    if tag in ATOMS:
        return ATOMS[tag]
    return f"?{tag}"


def main():
    root = ET.parse(sys.stdin).getroot()
    for src in root.iter("source"):
        for sf in src:
            print(conv(sf))


if __name__ == "__main__":
    main()
