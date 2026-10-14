; Rhombus forms are macros over shrubbery notation, so they are recognized by
; name and position rather than by dedicated nodes. Specific patterns come
; first: tree-sitter's own highlighter (and Helix) use the first match.

; Definitions
; ===========

(group
  .
  (identifier) @_def
  .
  (identifier) @function
  .
  (parens)
  (#any-of? @_def "fun" "method" "operator"))

(group
  .
  (identifier) @_def
  .
  (identifier) @function
  .
  (alternatives)
  (#any-of? @_def "fun" "method"))

(group
  .
  (identifier) @_def
  .
  (identifier) @type.definition
  (#any-of? @_def "class" "interface" "veneer" "enum" "annot" "syntax_class"))

(group
  .
  (identifier) @_def
  .
  (identifier) @variable
  (#any-of? @_def "def" "let"))

; At-notation
; ===========

(at_expression
  "@" @punctuation.special)

(at_expression
  command: (identifier) @function.call)

(at_command
  (identifier) @function.call .)

(text) @string

(text_comment) @comment

; Keywords
; ========

((identifier) @keyword
  (#any-of? @keyword
    "fun" "def" "let" "class" "interface" "veneer" "enum" "method" "property"
    "field" "constructor" "extends" "implements" "override" "private"
    "protected" "public" "final" "abstract" "mutable" "immutable" "nonfinal"
    "internal" "opaque" "binding" "annotation" "dot" "static" "authentic"
    "import" "export" "module" "namespace" "open" "only" "except" "rename"
    "as" "meta" "lib" "file" "use_static" "use_dynamic" "operator" "macro"
    "expr" "defn" "decl" "bind" "annot" "repet" "syntax_class" "pattern"
    "kind" "fields" "root_swap" "description" "error_mode" "match_def"
    "match_when" "match_unless" "default" "block" "begin" "values"))

((identifier) @keyword.conditional
  (#any-of? @keyword.conditional "if" "cond" "when" "unless" "match"))

((identifier) @keyword.repeat
  (#any-of? @keyword.repeat
    "for" "each" "while" "recur" "into" "keep_when" "skip_when" "break_when"
    "final_when"))

((identifier) @keyword.exception
  (#any-of? @keyword.exception "try" "throw" "catch" "finally" "initially"))

((identifier) @keyword.return
  (#any-of? @keyword.return "return" "break" "continue"))

((identifier) @variable.builtin
  (#any-of? @variable.builtin "this" "super"))

(keyword) @label

; Types and calls
; ===============

; `expr :: Annot` and `expr :~ Annot`
((operator) @_ann
  .
  (identifier) @type
  (#any-of? @_ann "::" ":~"))

((identifier) @type
  (#match? @type "^[A-Z]"))

((identifier) @function.call
  .
  (parens))

(identifier) @variable

; Literals
; ========

(string) @string
(byte_string) @string
(sexp_escape) @string.special
(number) @number
(boolean) @boolean
(void) @constant.builtin

; Punctuation and operators
; =========================

; `$` escapes inside syntax quotes
((operator) @punctuation.special
  (#eq? @punctuation.special "$"))

(operator) @operator

[
  "("
  ")"
  "["
  "]"
  "{"
  "}"
  "«"
  "»"
] @punctuation.bracket

"'" @punctuation.special

[
  ","
  ";"
  ":"
  "|"
] @punctuation.delimiter

; Comments
; ========

[
  (comment)
  (group_comment)
] @comment

(lang_line) @keyword.directive
