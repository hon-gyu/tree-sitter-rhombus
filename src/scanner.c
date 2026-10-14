// External scanner for shrubbery layout.
//
// The scanner keeps a stack of open group sequences. Each entry records what
// opened the sequence and the column of its first group:
//
//   BLOCK    after `:` (and the whole file)
//   ALT      after `|`
//   BRACKET  inside `()`, `[]` or `{}`
//   QUOTE    inside `'...'`
//   GUILL    inside `:«...»`, `|«...»` or `;«...»`, where line and column
//            are ignored
//   QGUILL   inside `'«...»'`, which is layout-sensitive like QUOTE
//
// An ALT also remembers whether it is still on the line where its `|` was:
// until a newline, another `|` ends the alternative instead of starting
// alternatives for the innermost group. Because scanner state only persists
// through emitted tokens, crossing a newline emits NEWLINE_MARK (a zero-width
// extra) to clear that flag.
//
// All layout tokens are zero-width. The scanner looks past whitespace and
// comments to the next real token, then decides from its character, column
// and whether a newline was crossed.

#include "tree_sitter/alloc.h"
#include "tree_sitter/array.h"
#include "tree_sitter/parser.h"

#include <stdbool.h>
#include <stdint.h>
#include <string.h>
#include <wctype.h>

enum TokenType {
  BLOCK_START,
  ALT_START,
  BRACKET_START,
  QUOTE_START,
  GUILL_START,
  SPLICE_START,
  LAYOUT_END,
  LAYOUT_SEP,
  OPERATOR,
  NEWLINE_MARK,
  ERROR_SENTINEL,
};

typedef enum {
  CTX_BLOCK,
  CTX_ALT,
  CTX_BRACKET,
  CTX_QUOTE,
  CTX_GUILL,
  CTX_QGUILL,
} CtxKind;

typedef struct {
  uint8_t kind;
  bool same_line;
  uint16_t col;
} Ctx;

typedef struct {
  Array(Ctx) stack;
} Scanner;

#define LGUILL 0xAB
#define RGUILL 0xBB

// Character classes
// =================

static bool is_opchar(int32_t c) {
  if (c < 128) {
    return c > 0 && strchr("!$%&*+-./<=>?^`|~:", c) != NULL;
  }
  if (c == LGUILL || c == RGUILL) return false;
  return (c >= 0xA1 && c <= 0xBF) || c == 0xD7 || c == 0xF7 ||
         (c >= 0x2010 && c <= 0x2BFF);
}

static bool is_closer(int32_t c) {
  return c == ')' || c == ']' || c == '}' || c == RGUILL;
}

// Stack helpers
// =============

static inline bool is_layout(Ctx *ctx) {
  return ctx->kind == CTX_BLOCK || ctx->kind == CTX_ALT;
}

static inline Ctx *top(Scanner *s) {
  return s->stack.size > 0 ? array_back(&s->stack) : NULL;
}

// The innermost context that is not a `:` block or `|` alternative
static Ctx *innermost_delimited(Scanner *s) {
  for (uint32_t i = s->stack.size; i > 0; i--) {
    Ctx *ctx = array_get(&s->stack, i - 1);
    if (!is_layout(ctx)) return ctx;
  }
  return NULL;
}

static bool closer_matches(Ctx *ctx, int32_t c) {
  switch (ctx->kind) {
    case CTX_BRACKET: return c == ')' || c == ']' || c == '}';
    case CTX_GUILL:
    case CTX_QGUILL: return c == RGUILL;
    case CTX_QUOTE: return c == '\'';
    default: return false;
  }
}

static bool push(Scanner *s, TSLexer *lexer, CtxKind kind, uint32_t col, enum TokenType tok) {
  Ctx ctx = {
    .kind = kind,
    .same_line = kind == CTX_ALT,
    .col = (uint16_t)(col > UINT16_MAX ? UINT16_MAX : col),
  };
  array_push(&s->stack, ctx);
  lexer->result_symbol = tok;
  return true;
}

static bool pop(Scanner *s, TSLexer *lexer) {
  array_pop(&s->stack);
  lexer->result_symbol = LAYOUT_END;
  return true;
}

// The innermost ALT reachable without crossing a delimited context
static Ctx *innermost_alt(Scanner *s) {
  for (uint32_t i = s->stack.size; i > 0; i--) {
    Ctx *ctx = array_get(&s->stack, i - 1);
    if (!is_layout(ctx)) return NULL;
    if (ctx->kind == CTX_ALT) return ctx;
  }
  return NULL;
}

// Lookahead past whitespace and comments
// ======================================

typedef struct {
  int32_t c;       // first character of the next real token
  uint32_t col;    // its column
  bool newline;    // a (non-escaped) newline was crossed
  bool space;      // whitespace or a comment was crossed
  bool dirty;      // characters were consumed without skipping (comments)
  bool consumed;   // `c` itself has already been consumed (a leading `/`)
  bool eof;
} Peek;

static void adv(TSLexer *lexer, Peek *p) {
  lexer->advance(lexer, !p->dirty);
}

// Shrubbery block comments nest, but the grammar's `comment` regex cannot, so
// this stops at the first `*/` to agree with it
static void skip_block_comment(TSLexer *lexer) {
  // positioned just after `/*`
  while (!lexer->eof(lexer)) {
    int32_t c = lexer->lookahead;
    lexer->advance(lexer, false);
    if (c == '*' && lexer->lookahead == '/') {
      lexer->advance(lexer, false);
      return;
    }
  }
}

static Peek peek_token(TSLexer *lexer) {
  Peek p = {0};
  bool backslash = false;
  for (;;) {
    int32_t c = lexer->lookahead;
    if (lexer->eof(lexer)) {
      p.eof = true;
      p.col = lexer->get_column(lexer);
      return p;
    }
    if (c == ' ' || c == '\t' || c == '\r' || c == '\f' || c == '\v') {
      p.space = true;
      adv(lexer, &p);
    } else if (c == '\n') {
      if (backslash) {
        backslash = false;
      } else {
        p.newline = true;
      }
      p.space = true;
      adv(lexer, &p);
    } else if (c == '\\') {
      backslash = true;
      adv(lexer, &p);
    } else if (c == '/') {
      uint32_t col = lexer->get_column(lexer);
      lexer->advance(lexer, false);
      if (lexer->lookahead == '/') {
        p.dirty = p.space = true;
        while (!lexer->eof(lexer) && lexer->lookahead != '\n') {
          lexer->advance(lexer, false);
        }
      } else if (lexer->lookahead == '*') {
        p.dirty = p.space = true;
        lexer->advance(lexer, false);
        skip_block_comment(lexer);
      } else {
        p.c = '/';
        p.col = col;
        p.consumed = true;
        return p;
      }
    } else if (c == '#') {
      // `#lang ...` and `#!...` lines are comments as far as layout goes
      uint32_t col = lexer->get_column(lexer);
      lexer->advance(lexer, false);
      bool line = false;
      if (lexer->lookahead == '!') {
        line = true;
      } else if (lexer->lookahead == 'l') {
        line = true;
        for (const char *k = "lang"; *k; k++) {
          if (lexer->lookahead != *k) {
            line = false;
            break;
          }
          lexer->advance(lexer, false);
        }
      }
      if (!line) {
        p.c = '#';
        p.col = col;
        p.consumed = true;
        return p;
      }
      p.dirty = p.space = true;
      while (!lexer->eof(lexer) && lexer->lookahead != '\n') {
        lexer->advance(lexer, false);
      }
    } else {
      p.c = c;
      p.col = lexer->get_column(lexer);
      return p;
    }
  }
}

// Operators
// =========

static bool is_digit(int32_t c) { return c >= '0' && c <= '9'; }

// An operator is a run of operator characters that does not contain `//` or
// `/*`, is not `|`, `:` or `~` alone, and does not end in `:` unless it is all
// colons. `#` followed by one of `',:;|` is also an operator.
static bool scan_operator(TSLexer *lexer, Peek *p, bool after_term) {
  if (p->dirty || p->eof) return false;

  if (p->c == '#') {
    if (!p->consumed) lexer->advance(lexer, false);
    if (strchr("',:;|", lexer->lookahead) != NULL && lexer->lookahead != 0) {
      lexer->advance(lexer, false);
      lexer->mark_end(lexer);
      lexer->result_symbol = OPERATOR;
      return true;
    }
    return false;
  }

  if (!is_opchar(p->c)) return false;
  if (!p->consumed) lexer->advance(lexer, false);

  // A sign directly before a digit is part of the number, unless the sign
  // immediately follows a term (`x-1` is `x - 1`, but `x -1` is `x` and `-1`)
  if ((p->c == '+' || p->c == '-') && is_digit(lexer->lookahead) &&
      (p->space || !after_term)) {
    return false;
  }

  int32_t first = p->c, last = p->c;
  bool all_colons = first == ':';
  unsigned len = 1;
  bool marked = false;

  for (;;) {
    bool ok = len == 1 ? (first != '|' && first != ':' && first != '~')
                       : (last != ':' || all_colons);
    if (ok) {
      lexer->mark_end(lexer);
      marked = true;
    }

    int32_t c = lexer->lookahead;
    if (!is_opchar(c)) break;
    lexer->advance(lexer, false);
    if (c == '/' && (lexer->lookahead == '/' || lexer->lookahead == '*')) break;
    if (c != ':') all_colons = false;
    last = c;
    len++;
  }

  if (marked) lexer->result_symbol = OPERATOR;
  return marked;
}

// Main entry
// ==========

static bool scan(Scanner *s, TSLexer *lexer, const bool *valid) {
  if (valid[ERROR_SENTINEL]) return false;

  lexer->mark_end(lexer);
  Peek p = peek_token(lexer);
  Ctx *ctx = top(s);
  Ctx *delimited = innermost_delimited(s);
  bool quote_closer = p.c == '\'' && delimited && delimited->kind == CTX_QUOTE;

  // A lone `|` (not part of an operator like `||` or `|>`)
  bool bar = false;
  if (p.c == '|' && !p.eof) {
    lexer->advance(lexer, false);
    p.consumed = true;
    bar = !is_opchar(lexer->lookahead);
  }

  // Leaving the line of open alternatives
  // -------------------------------------

  if (valid[NEWLINE_MARK] && p.newline) {
    bool marked = false;
    for (uint32_t i = s->stack.size; i > 0; i--) {
      Ctx *c = array_get(&s->stack, i - 1);
      if (!is_layout(c)) break;
      if (c->same_line) {
        c->same_line = false;
        marked = true;
      }
    }
    if (marked) {
      lexer->result_symbol = NEWLINE_MARK;
      return true;
    }
  }

  // Opening a sequence
  // ------------------

  if (valid[BLOCK_START] || valid[ALT_START]) {
    bool empty = p.eof || is_closer(p.c) || p.c == ',' || p.c == ';' ||
                 p.c == LGUILL || quote_closer ||
                 (p.newline && ctx && p.col <= ctx->col);
    if (!empty) {
      return push(s, lexer, valid[BLOCK_START] ? CTX_BLOCK : CTX_ALT, p.col,
                  valid[BLOCK_START] ? BLOCK_START : ALT_START);
    }
  }

  if (valid[BRACKET_START] && !p.eof && !is_closer(p.c)) {
    return push(s, lexer, CTX_BRACKET, p.col, BRACKET_START);
  }
  if (valid[QUOTE_START]) {
    return push(s, lexer, CTX_QUOTE, p.col, QUOTE_START);
  }
  if (valid[GUILL_START] && !p.eof && p.c != RGUILL) {
    CtxKind kind = ctx && ctx->kind == CTX_QUOTE ? CTX_QGUILL : CTX_GUILL;
    return push(s, lexer, kind, p.col, GUILL_START);
  }
  if (valid[SPLICE_START] && !p.eof && p.c != RGUILL) {
    return push(s, lexer, CTX_GUILL, p.col, SPLICE_START);
  }

  // Closing a sequence
  // ------------------

  if (valid[LAYOUT_END] && ctx) {
    if (p.eof) return pop(s, lexer);

    if (is_closer(p.c) || quote_closer) {
      if (is_layout(ctx) || closer_matches(ctx, p.c)) return pop(s, lexer);
    }

    // `,` closes blocks up to the enclosing bracket
    if (p.c == ',' && is_layout(ctx) && delimited && delimited->kind == CTX_BRACKET) {
      return pop(s, lexer);
    }

    // `a | b: c | d`: a `|` on the line of the current alternative's `|`
    // ends that alternative
    Ctx *alt = innermost_alt(s);
    if (bar && !p.newline && alt && alt->same_line) return pop(s, lexer);

    // dedent
    if (p.newline && is_layout(ctx) && p.col < ctx->col) return pop(s, lexer);
  }

  // Separating groups
  // -----------------

  if (valid[LAYOUT_SEP] && ctx && p.newline && !p.eof && !bar &&
      (is_layout(ctx) || ctx->kind == CTX_QUOTE || ctx->kind == CTX_QGUILL) &&
      p.col == ctx->col) {
    lexer->result_symbol = LAYOUT_SEP;
    return true;
  }

  // Otherwise the next token continues the current group
  // After a term the group may end, so LAYOUT_END is valid; at the start of a
  // group it is not
  if (valid[OPERATOR] && !bar) return scan_operator(lexer, &p, valid[LAYOUT_END]);
  return false;
}

// Tree-sitter interface
// =====================

void *tree_sitter_rhombus_external_scanner_create(void) {
  Scanner *s = ts_calloc(1, sizeof(Scanner));
  array_init(&s->stack);
  return s;
}

void tree_sitter_rhombus_external_scanner_destroy(void *payload) {
  Scanner *s = payload;
  array_delete(&s->stack);
  ts_free(s);
}

unsigned tree_sitter_rhombus_external_scanner_serialize(void *payload, char *buffer) {
  Scanner *s = payload;
  unsigned n = 0;
  for (uint32_t i = 0; i < s->stack.size; i++) {
    if (n + 3 > TREE_SITTER_SERIALIZATION_BUFFER_SIZE) break;
    Ctx *ctx = array_get(&s->stack, i);
    buffer[n++] = (char)(ctx->kind | (ctx->same_line ? 0x80 : 0));
    buffer[n++] = (char)(ctx->col & 0xFF);
    buffer[n++] = (char)(ctx->col >> 8);
  }
  return n;
}

void tree_sitter_rhombus_external_scanner_deserialize(void *payload, const char *buffer, unsigned length) {
  Scanner *s = payload;
  array_clear(&s->stack);
  for (unsigned i = 0; i + 3 <= length; i += 3) {
    Ctx ctx = {
      .kind = (uint8_t)buffer[i] & 0x7F,
      .same_line = ((uint8_t)buffer[i] & 0x80) != 0,
      .col = (uint16_t)((uint8_t)buffer[i + 1] | ((uint8_t)buffer[i + 2] << 8)),
    };
    array_push(&s->stack, ctx);
  }
}

bool tree_sitter_rhombus_external_scanner_scan(void *payload, TSLexer *lexer, const bool *valid_symbols) {
  return scan(payload, lexer, valid_symbols);
}
