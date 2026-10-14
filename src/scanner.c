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
//   TEXT     inside at-notation text `{...}` or `|<<{...}>>|`, which is
//            lexed as raw text up to an escape (`@` or `|<<@`) or the closer,
//            or a whole module body under a Scribble `#lang`
//
// A BLOCK or ALT opened directly inside GUILL is `free`: like GUILL, it
// ignores newlines and columns.
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
  TEXT_OPEN,
  TEXT_CONTENT,
  TEXT_ESCAPE,
  TEXT_CLOSE,
  TEXT_COMMENT,
  AT_ARGS_OPEN,
  AT_COMMAND_OPERATOR,
  TEXT_LANG_LINE,
  LINE_CONTINUATION,
  ERROR_SENTINEL,
};

typedef enum {
  CTX_BLOCK,
  CTX_ALT,
  CTX_BRACKET,
  CTX_QUOTE,
  CTX_GUILL,
  CTX_QGUILL,
  CTX_TEXT,
} CtxKind;

#define MAX_DELIM 16

typedef struct {
  uint8_t kind;
  bool same_line;
  bool free;
  uint16_t col;  // for TEXT: nesting depth of balanced openers in the text
  // For TEXT opened by `|<<{`: `custom` is set and `delim` holds `<<`
  bool custom;
  bool document;  // TEXT for a whole module body, which ends only at EOF
  uint8_t len;
  char delim[MAX_DELIM];
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

static Ctx *innermost_delimited(Scanner *s);

static bool push(Scanner *s, TSLexer *lexer, CtxKind kind, uint32_t col, enum TokenType tok) {
  Ctx *delimited = innermost_delimited(s);
  Ctx ctx = {
    .kind = kind,
    .same_line = kind == CTX_ALT,
    .free = (kind == CTX_BLOCK || kind == CTX_ALT) && delimited && delimited->kind == CTX_GUILL,
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
  bool text_lang;  // a `#lang` for a Scribble language was read and marked
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

// With `want_lang`, a `#lang` line naming a Scribble language stops the peek
// with the token end marked after it
static Peek peek_token(TSLexer *lexer, bool want_lang) {
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
      if (!backslash) p.newline = true;
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
      const char *want = "scribble";
      const char *k = want;
      while (!lexer->eof(lexer) && lexer->lookahead != '\n') {
        if (*k) k = lexer->lookahead == *k ? k + 1 : (lexer->lookahead == want[0] ? want + 1 : want);
        lexer->advance(lexer, false);
      }
      if (want_lang && !*k) {
        lexer->mark_end(lexer);
        p.text_lang = true;
        return p;
      }
    } else {
      p.c = c;
      p.col = lexer->get_column(lexer);
      return p;
    }
  }
}

// Consume `\` and the whitespace and comments after it, up to the next token
static bool scan_line_continuation(TSLexer *lexer) {
  lexer->advance(lexer, false);
  lexer->mark_end(lexer);
  for (;;) {
    int32_t c = lexer->lookahead;
    if (c == ' ' || c == '\t' || c == '\r' || c == '\f' || c == '\v' || c == '\n') {
      lexer->advance(lexer, false);
    } else if (c == '/') {
      lexer->advance(lexer, false);
      if (lexer->lookahead == '/') {
        while (!lexer->eof(lexer) && lexer->lookahead != '\n') lexer->advance(lexer, false);
      } else if (lexer->lookahead == '*') {
        lexer->advance(lexer, false);
        skip_block_comment(lexer);
      } else {
        break;  // an operator, which the marked end excludes
      }
    } else {
      break;
    }
    lexer->mark_end(lexer);
  }
  lexer->result_symbol = LINE_CONTINUATION;
  return true;
}

// Operators
// =========

static bool is_digit(int32_t c) { return c >= '0' && c <= '9'; }

static bool is_ident_start(int32_t c) {
  if (c < 128) return iswalpha(c) || c == '_';
  return !is_opchar(c) && c != LGUILL && c != RGUILL && iswalpha(c);
}

// An operator is a run of operator characters that does not contain `//` or
// `/*`, is not `|`, `:` or `~` alone, and does not end in `:` unless it is all
// colons. `#` followed by one of `',:;|` is also an operator.
//
// `ident_follows` reports whether an identifier starts right after it, which is
// how `@a.b.c` commands continue.
static bool scan_operator(TSLexer *lexer, Peek *p, bool after_term, bool *ident_follows) {
  *ident_follows = false;
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

  // A sign or `.` directly before a digit is part of the number, unless it
  // immediately follows a term (`x-1` is `x - 1`, but `x -1` is `x` and `-1`)
  if ((p->c == '+' || p->c == '-' || p->c == '.') && is_digit(lexer->lookahead) &&
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
    if (!is_opchar(c)) {
      *ident_follows = ok && is_ident_start(c);
      break;
    }
    lexer->advance(lexer, false);
    if (c == '/' && (lexer->lookahead == '/' || lexer->lookahead == '*')) break;
    if (c != ':') all_colons = false;
    last = c;
    len++;
  }

  if (marked) lexer->result_symbol = OPERATOR;
  return marked;
}

// At-notation text
// ================

static bool is_delim_char(int32_t c) {
  return c > ' ' && c < 127 && !iswalnum(c) && c != '{' && c != '@' && c != '_';
}

static int32_t flip(int32_t c) {
  switch (c) {
    case '(': return ')';
    case ')': return '(';
    case '[': return ']';
    case ']': return '[';
    case '<': return '>';
    case '>': return '<';
    default: return c;
  }
}

// Lex `{` or `|<<{` and enter text mode
static bool scan_text_open(Scanner *s, TSLexer *lexer) {
  Ctx ctx = {.kind = CTX_TEXT};
  if (lexer->lookahead == '|') {
    ctx.custom = true;
    lexer->advance(lexer, false);
    while (is_delim_char(lexer->lookahead)) {
      if (ctx.len == MAX_DELIM) return false;
      ctx.delim[ctx.len++] = (char)lexer->lookahead;
      lexer->advance(lexer, false);
    }
  }
  if (lexer->lookahead != '{') return false;
  lexer->advance(lexer, false);
  lexer->mark_end(lexer);
  array_push(&s->stack, ctx);
  lexer->result_symbol = TEXT_OPEN;
  return true;
}

// After a `|`, match the `<<` of a custom opener or escape
static bool match_delim(TSLexer *lexer, Ctx *t) {
  for (unsigned i = 0; i < t->len; i++) {
    if (lexer->lookahead != t->delim[i]) return false;
    lexer->advance(lexer, false);
  }
  return true;
}

// After a `}`, match the `>>|` of a custom closer
static bool match_closer_rest(TSLexer *lexer, Ctx *t) {
  if (!t->custom) return true;
  for (unsigned i = t->len; i > 0; i--) {
    if (lexer->lookahead != flip(t->delim[i - 1])) return false;
    lexer->advance(lexer, false);
  }
  if (lexer->lookahead != '|') return false;
  lexer->advance(lexer, false);
  return true;
}

// Positioned after an escape: emit it, or a whole `@//...` comment
static bool finish_escape(TSLexer *lexer) {
  lexer->mark_end(lexer);
  lexer->result_symbol = TEXT_ESCAPE;
  if (lexer->lookahead != '/') return true;
  lexer->advance(lexer, false);
  if (lexer->lookahead != '/') return true;
  lexer->advance(lexer, false);

  // `@//{...}` or `@//|<<{...}>>|` comments out balanced text
  Ctx text = {.kind = CTX_TEXT};
  bool braced = lexer->lookahead == '{';
  if (lexer->lookahead == '|') {
    text.custom = true;
    lexer->advance(lexer, false);
    while (is_delim_char(lexer->lookahead) && text.len < MAX_DELIM) {
      text.delim[text.len++] = (char)lexer->lookahead;
      lexer->advance(lexer, false);
    }
    braced = lexer->lookahead == '{';
  }
  if (braced) {
    lexer->advance(lexer, false);
    unsigned depth = 0;
    while (!lexer->eof(lexer)) {
      int32_t c = lexer->lookahead;
      lexer->advance(lexer, false);
      if (c == '}' && match_closer_rest(lexer, &text)) {
        if (depth == 0) break;
        depth--;
      } else if (text.custom ? c == '|' && match_delim(lexer, &text) && lexer->lookahead == '{'
                             : c == '{') {
        if (text.custom) lexer->advance(lexer, false);
        depth++;
      }
    }
  } else {
    // `@//...` comments out the rest of the line, its newline, and the next
    // line's indentation
    while (!lexer->eof(lexer) && lexer->lookahead != '\n') lexer->advance(lexer, false);
    if (lexer->lookahead == '\n') lexer->advance(lexer, false);
    while (lexer->lookahead == ' ' || lexer->lookahead == '\t') lexer->advance(lexer, false);
  }
  lexer->mark_end(lexer);
  lexer->result_symbol = TEXT_COMMENT;
  return true;
}

// Inside text, whitespace is content, so nothing is skipped
static bool scan_text(Scanner *s, TSLexer *lexer, const bool *valid) {
  Ctx *t = top(s);
  if (lexer->eof(lexer) && !t->document) return false;
  int32_t c = lexer->lookahead;
  bool any = false;

  // Parts of an escape `@command(args){text}` directly after the command
  if (valid[AT_ARGS_OPEN] && c == '(') {
    lexer->advance(lexer, false);
    lexer->mark_end(lexer);
    lexer->result_symbol = AT_ARGS_OPEN;
    return true;
  }
  if (valid[TEXT_OPEN] && (c == '{' || (c == '|' && !t->custom))) {
    if (scan_text_open(s, lexer)) return true;
    any = true;
    lexer->mark_end(lexer);
  } else if (valid[AT_COMMAND_OPERATOR] && is_opchar(c) && !(t->custom && c == '|')) {
    Peek p = {.c = c};
    bool ident;
    if (scan_operator(lexer, &p, true, &ident) && ident) {
      lexer->result_symbol = AT_COMMAND_OPERATOR;
      return true;
    }
    any = true;
    lexer->mark_end(lexer);
  }

  uint16_t depth = t->col;
  while (!lexer->eof(lexer)) {
    c = lexer->lookahead;

    if (c == '}' && t->document && depth == 0) {
      lexer->advance(lexer, false);
      any = true;
      lexer->mark_end(lexer);
      continue;
    }

    if (c == '}') {
      lexer->advance(lexer, false);
      if (match_closer_rest(lexer, t)) {
        if (depth == 0) {
          if (any) break;
          lexer->mark_end(lexer);
          array_pop(&s->stack);
          lexer->result_symbol = TEXT_CLOSE;
          return true;
        }
        depth--;
      }
      any = true;
      lexer->mark_end(lexer);
      continue;
    }

    if (t->custom ? c == '|' : (c == '@' || c == '{')) {
      lexer->advance(lexer, false);
      int32_t next = t->custom ? (match_delim(lexer, t) ? lexer->lookahead : 0) : c;
      if (next == '@') {
        if (any) break;
        if (t->custom) lexer->advance(lexer, false);
        return finish_escape(lexer);
      }
      if (next == '{') {
        if (t->custom) lexer->advance(lexer, false);
        depth++;
      }
      any = true;
      lexer->mark_end(lexer);
      continue;
    }

    lexer->advance(lexer, false);
    any = true;
    lexer->mark_end(lexer);
  }

  if (!any && t->document && lexer->eof(lexer) && valid[TEXT_CLOSE]) {
    array_pop(&s->stack);
    lexer->result_symbol = TEXT_CLOSE;
    return true;
  }
  if (!any || !valid[TEXT_CONTENT]) return false;
  t->col = depth;
  lexer->result_symbol = TEXT_CONTENT;
  return true;
}

// Main entry
// ==========

static bool scan(Scanner *s, TSLexer *lexer, const bool *valid) {
  if (valid[ERROR_SENTINEL]) return false;

  lexer->mark_end(lexer);
  Ctx *ctx = top(s);
  if (ctx && ctx->kind == CTX_TEXT && (valid[TEXT_CONTENT] || valid[TEXT_CLOSE])) {
    return scan_text(s, lexer, valid);
  }

  // Spaces before a `\` are skipped so the continuation token starts at it;
  // other tokens are unaffected by where on the line they start
  bool space = false;
  while (lexer->lookahead == ' ' || lexer->lookahead == '\t') {
    lexer->advance(lexer, true);
    space = true;
  }
  if (lexer->lookahead == '\\' && valid[LINE_CONTINUATION]) {
    return scan_line_continuation(lexer);
  }
  lexer->mark_end(lexer);

  Peek p = peek_token(lexer, valid[TEXT_LANG_LINE]);
  p.space |= space;
  // Inside `«»` newlines are just whitespace
  if (ctx && (ctx->kind == CTX_GUILL || ctx->free)) p.newline = false;
  if (p.text_lang) {
    Ctx doc = {.kind = CTX_TEXT, .document = true};
    array_push(&s->stack, doc);
    lexer->result_symbol = TEXT_LANG_LINE;
    return true;
  }
  Ctx *delimited = innermost_delimited(s);
  bool quote_closer = p.c == '\'' && delimited && delimited->kind == CTX_QUOTE;

  // At-notation parts that must directly follow `@` or the command
  if (!p.space && !p.eof && !p.consumed) {
    if (valid[TEXT_OPEN] && (p.c == '{' || p.c == '|')) {
      if (scan_text_open(s, lexer)) return true;
      if (p.c == '|') return false;
    }
    if (valid[AT_ARGS_OPEN] && p.c == '(') {
      lexer->advance(lexer, false);
      lexer->mark_end(lexer);
      lexer->result_symbol = AT_ARGS_OPEN;
      return true;
    }
  }

  // A lone `|` (not part of an operator like `||` or `|>`)
  bool bar = false;
  if (p.c == '|' && !p.eof) {
    lexer->advance(lexer, false);
    p.consumed = true;
    bar = !is_opchar(lexer->lookahead);
  }

  // `#//` commenting out an alternative acts like that alternative's `|`. On
  // its own line, `#//` does not count for indentation, so the `|` sets it.
  if (p.c == '#' && p.consumed && lexer->lookahead == '/') {
    lexer->advance(lexer, false);
    if (lexer->lookahead == '/') {
      lexer->advance(lexer, false);
      bool newline = false;
      while (iswspace(lexer->lookahead) && !lexer->eof(lexer)) {
        if (lexer->lookahead == '\n') newline = true;
        lexer->advance(lexer, false);
      }
      if (lexer->lookahead == '|') {
        uint32_t col = lexer->get_column(lexer);
        lexer->advance(lexer, false);
        if (!is_opchar(lexer->lookahead)) {
          bar = true;
          if (newline) p.col = col;
        }
      }
    }
  }

  // Leaving the line of open alternatives
  // -------------------------------------

  // A newline inside brackets does not end the line of an alternative. So the
  // mark waits while a sequence is being opened (the newline may belong to the
  // new context), and is skipped before a closer (the newline belonged to the
  // context being closed)
  bool opening = valid[BLOCK_START] || valid[ALT_START] || valid[BRACKET_START] ||
                 valid[QUOTE_START] || valid[GUILL_START] || valid[SPLICE_START];
  bool closing = is_closer(p.c) || quote_closer;
  if (valid[NEWLINE_MARK] && p.newline && !opening && !closing) {
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
      // BRACKET only for at-notation arguments, which allow newline separators
      (is_layout(ctx) || ctx->kind == CTX_QUOTE || ctx->kind == CTX_QGUILL ||
       ctx->kind == CTX_BRACKET) &&
      p.col == ctx->col) {
    lexer->result_symbol = LAYOUT_SEP;
    return true;
  }

  // Otherwise the next token continues the current group
  // After a term the group may end, so LAYOUT_END is valid; at the start of a
  // group it is not
  if ((valid[OPERATOR] || valid[AT_COMMAND_OPERATOR]) && !bar) {
    bool ident;
    if (!scan_operator(lexer, &p, valid[LAYOUT_END], &ident)) return false;
    if (valid[AT_COMMAND_OPERATOR] && !p.space && ident) {
      lexer->result_symbol = AT_COMMAND_OPERATOR;
    } else if (!valid[OPERATOR]) {
      return false;
    }
    return true;
  }
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
    Ctx *ctx = array_get(&s->stack, i);
    unsigned size = 3 + (ctx->kind == CTX_TEXT ? 1 + ctx->len : 0);
    if (n + size > TREE_SITTER_SERIALIZATION_BUFFER_SIZE) break;
    buffer[n++] = (char)(ctx->kind | (ctx->same_line ? 0x80 : 0) | (ctx->free ? 0x40 : 0));
    buffer[n++] = (char)(ctx->col & 0xFF);
    buffer[n++] = (char)(ctx->col >> 8);
    if (ctx->kind == CTX_TEXT) {
      buffer[n++] = (char)(ctx->len | (ctx->custom ? 0x80 : 0) | (ctx->document ? 0x40 : 0));
      memcpy(buffer + n, ctx->delim, ctx->len);
      n += ctx->len;
    }
  }
  return n;
}

void tree_sitter_rhombus_external_scanner_deserialize(void *payload, const char *buffer, unsigned length) {
  Scanner *s = payload;
  array_clear(&s->stack);
  unsigned i = 0;
  while (i + 3 <= length) {
    Ctx ctx = {
      .kind = (uint8_t)buffer[i] & 0x3F,
      .same_line = ((uint8_t)buffer[i] & 0x80) != 0,
      .free = ((uint8_t)buffer[i] & 0x40) != 0,
      .col = (uint16_t)((uint8_t)buffer[i + 1] | ((uint8_t)buffer[i + 2] << 8)),
    };
    i += 3;
    if (ctx.kind == CTX_TEXT && i < length) {
      ctx.custom = ((uint8_t)buffer[i] & 0x80) != 0;
      ctx.document = ((uint8_t)buffer[i] & 0x40) != 0;
      ctx.len = (uint8_t)buffer[i] & 0x3F;
      i++;
      memcpy(ctx.delim, buffer + i, ctx.len);
      i += ctx.len;
    }
    array_push(&s->stack, ctx);
  }
}

bool tree_sitter_rhombus_external_scanner_scan(void *payload, TSLexer *lexer, const bool *valid_symbols) {
  return scan(payload, lexer, valid_symbols);
}
