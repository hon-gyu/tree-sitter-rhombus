# tree-sitter-rhombus

> ai-disclosure: ai-generated

A [tree-sitter](https://tree-sitter.github.io/) grammar for [Rhombus](https://rhombus-lang.org/).

## Design

Rhombus is defined in two layers:

1. **[Shrubbery notation](https://docs.racket-lang.org/shrubbery/)**, a fixed
   syntax of groups, blocks (`:`), alternatives (`|`), brackets, quotes and
   at-notation.
2. **Rhombus itself**, whose forms (`fun`, `class`, `match`, operators, ...)
   are macros over shrubberies.

This grammar parses layer 1 only, so it works for any shrubbery-based
language and is unaffected by user-defined macros. Rhombus-specific knowledge
lives in `queries/`, which recognizes forms like `fun name(...)` by name and
position.

The tree mirrors the reference reader's structure: `group`, `block`,
`alternatives`/`alternative`, `parens`, `brackets`, `braces`, `quotes`,
`at_expression`/`text_block`, plus atoms.

Shrubbery is layout-sensitive, so most of the work happens in
`src/scanner.c`. It keeps a stack of open group sequences and emits
zero-width tokens that open, separate and close them. It also lexes
operators, at-notation text, and the cases where adjacency matters:
`x-1` vs `x -1`, `@f(x)` vs `@f (x)`.

## Status

The grammar is checked against the official shrubbery reader
(`shrubbery/parse` from Racket) on the Rhombus repository: all `.rhm` files,
plus the `.scrbl` docs, which are almost entirely at-notation. 1145 of 1147
files produce the same tree. One of the other two is the docs page that
demonstrates nested comments. The other differs only in how
`tools/ts2shrub.py` trims an empty text block (`{` followed by `}` on the
next line), not in the parse.

Supported:

- groups, `:` blocks, `|` alternatives, including the same-line `|` rules
- `()`, `[]`, `{}`, `'...'` quotes
- `«»` in all its forms (`:«»`, `|«»`, `;«»`, `'«»'`)
- `\` line continuation
- `#//` group and alternative comments
- at-notation: `@cmd(args){text}`, custom `|<<{...}>>|` delimiters,
  `@//` comments
- Scribble modules (`#lang rhombus/scribble/...`), whose body is parsed as
  text

Known limitations:

- **Nested block comments** (`/* a /* b */ c */`) end at the first `*/`.
- **`#{...}` S-expression escapes** don't balance nested braces.

## Editors

`queries/highlights.scm` follows tree-sitter's own convention, where the first
matching pattern wins. Editors that let the last pattern win, such as Zed and
Neovim, need the queries reordered.
