# Regenerate the parser from grammar.js
generate:
    tree-sitter generate

# Corpus tests and incremental-parsing fuzz
test: generate
    tree-sitter test
    tree-sitter fuzz --edits 5 --iterations 20

# Diff against the official shrubbery reader (run inside `devenv shell`)
compare *files: generate
    uv run --no-project python tools/compare.py {{ if files == "" { "$(find rhombus -name '*.rhm')" } else { files } }}
