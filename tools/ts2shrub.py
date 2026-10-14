"""Convert `tree-sitter parse -x` output into the normalized form of tools/ref.rkt.

Usage: tree-sitter parse -x FILE | uv run tools/ts2shrub.py
"""

import os
import re
import sys
import xml.etree.ElementTree as ET

SEQ = {"parens", "brackets", "braces", "quotes", "block"}
SKIP = {"comment", "group_comment", "alternative_comment", "lang_line", "text_comment", "line_continuation"}
STR = "(group str)"

# Source lines (as bytes) of the file being converted, for slicing raw text
source = []


def load(path):
    global source
    with open(path, "rb") as f:
        source = f.read().split(b"\n")


def raw(e):
    sr, sc, er, ec = (int(e.get(k)) for k in ("srow", "scol", "erow", "ecol"))
    if sr == er:
        return source[sr][sc:ec].decode()
    lines = [source[sr][sc:], *source[sr + 1 : er], source[er][:ec]]
    return b"\n".join(lines).decode()


def collapse(items):
    """Merge adjacent string elements; ref.rkt does the same."""
    out = []
    for item in items:
        if not (item == STR and out and out[-1] == STR):
            out.append(item)
    return out
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
        if any(c.tag in ("text", "at_expression") for c in e):
            # a module body read as text
            return "(" + " ".join(["brackets", *collapse(text_items(e))]) + ")"
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
    if tag == "brackets":
        return "(" + " ".join(["brackets", *collapse(map(conv, kids(e)))]) + ")"
    if tag in SEQ:
        return "(" + " ".join([tag, *map(conv, kids(e))]) + ")"
    if tag == "at_expression":
        return conv_at(e)
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
        if re.fullmatch(r"[-+0-9.e/i#xob]+", inner) and re.search(r"\d|^[-+]i$", inner):
            return "num"
        if inner.startswith("#:"):
            return "~" + inner[2:]
        if inner.startswith('"'):
            return "str"
        if inner.startswith("#") and not inner.startswith("#%"):
            return "?" + inner
        return prefix + inner.strip("|").replace("\\", "")
    if tag in ATOMS:
        return ATOMS[tag]
    return f"?{tag}"


# At-notation
# ===========


def field(e, name):
    return [c for c in e if c.get("field") == name]


def group_terms(g):
    # the `(group ...)` wrapper removed
    return conv(g)[len("(group ") : -1]


def conv_at(e):
    """`@cmd(args){text}` becomes `cmd(args, [text])`."""
    [cmd] = field(e, "command") or [None]
    args = field(e, "arguments")
    texts = field(e, "text")
    if cmd is None and not texts:
        # `@(«group»)`
        return " ".join(group_terms(g) for g in kids(e))

    terms = []
    if cmd is not None:
        if cmd.tag == "at_command" and any(c.tag == "group" for c in cmd):
            terms = [group_terms(g) for g in kids(cmd)]
        elif cmd.tag == "at_command":
            terms = [conv(c) for c in kids(cmd)]
        else:
            terms = [conv(cmd)]
    if not args and not texts:
        return " ".join(terms)

    parts = [conv(g) for a in args for g in kids(a)]
    for tb in texts:
        items = collapse(text_items(tb))
        parts.append("(group (brackets" + "".join(" " + i for i in items) + "))")
    terms.append("(parens" + "".join(" " + p for p in parts) + ")")
    return " ".join(terms)


def text_items(tb):
    """Mirror the reference's conversion of braced text into list elements."""
    # lines of items: ("t", str) or ("e", converted escape)
    lines = [{"items": [], "nl": False}]
    for c in tb:
        if c.tag == "text":
            for i, chunk in enumerate(raw(c).split("\n")):
                if i > 0:
                    lines[-1]["nl"] = True
                    lines.append({"items": [], "nl": False})
                if chunk:
                    lines[-1]["items"].append(("t", chunk))
        elif c.tag == "at_expression":
            lines[-1]["items"].append(("e", "(group " + conv(c) + ")"))

    def blank(line):
        return all(k == "t" and not v.strip() for k, v in line["items"])

    # single-line text is kept as-is
    if len(lines) == 1:
        return [v if k == "e" else STR for k, v in lines[0]["items"]]

    # trailing whitespace is dropped, except after content on the closer's line
    for i, line in enumerate(lines):
        its = line["items"]
        if its and its[-1][0] == "t" and (i < len(lines) - 1 or blank(line)):
            its[-1] = ("t", its[-1][1].rstrip(" \t\r"))
            if not its[-1][1]:
                its.pop()
    if len(lines) > 1 and blank(lines[0]):
        lines.pop(0)
    if lines and blank(lines[-1]) and (len(lines) > 1 or not lines[0]["items"]):
        lines.pop()
    if lines and lines[-1]["nl"] and lines[-1]["items"]:
        lines[-1]["nl"] = False

    # leading whitespace becomes its own element, minus the shared indentation
    leads = []
    for line in lines:
        its = line["items"]
        lead = ""
        if its and its[0][0] == "t":
            text = its[0][1]
            lead = text[: len(text) - len(text.lstrip(" \t"))]
            rest = text[len(lead) :]
            its[0:1] = [("t", rest)] if rest else []
        leads.append(lead)
    indents = [lead for lead, line in zip(leads, lines) if line["items"] or lead]
    shared = os.path.commonprefix(indents) if indents else ""

    out = []
    for lead, line in zip(leads, lines):
        if lead[len(shared) :]:
            out.append(STR)
        for k, v in line["items"]:
            out.append(v if k == "e" else STR)
        if line["nl"]:
            out.append(STR)
    return out


def main():
    root = ET.parse(sys.stdin).getroot()
    for src in root.iter("source"):
        load(src.get("name"))
        for sf in src:
            print(conv(sf))


if __name__ == "__main__":
    main()
