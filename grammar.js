/**
 * @file Tree-sitter grammar for Rhombus, parsed at the level of shrubbery notation
 * @license MIT
 */

/// <reference types="tree-sitter-cli/dsl" />
// @ts-check

// Shrubbery notation is layout-sensitive. The external scanner (src/scanner.c)
// tracks a stack of group-sequence contexts and emits zero-width tokens that
// open (`_*_start`), separate (`_layout_sep`) and close (`_layout_end`) them.
//
// The grammar deliberately stops at shrubbery structure: Rhombus forms such as
// `fun`, `class` or `match` are macros, so they are recognized in queries rather
// than in the grammar.

module.exports = grammar({
  name: 'rhombus',

  externals: $ => [
    $._block_start,
    $._alt_start,
    $._bracket_start,
    $._quote_start,
    $._guillemet_start,
    $._splice_start,
    $._layout_end,
    $._layout_sep,
    $.operator,
    $._newline_mark,
    // at-notation
    $._text_open,
    $.text,
    $._text_escape,
    $._text_close,
    $.text_comment,
    $._at_args_open,
    $._at_command_operator,
    $._text_lang_line,
    $.line_continuation,
    $._error_sentinel,
  ],

  // `line_continuation` is a `\` plus any whitespace and comments up to the
  // next token, so the scanner sees them as one. A `\` the scanner does not
  // reach first (one starting a line) is plain whitespace.
  extras: $ => [/\s/, $.comment, $.lang_line, $.line_continuation, /\\/, $._newline_mark],

  rules: {
    source_file: $ => choice(
      optional(seq($._block_start, $._groups, $._layout_end)),
      $._text_document,
    ),

    // A `#lang` naming a Scribble language (like `rhombus/scribble/manual`)
    // reads the whole module body as at-notation text
    _text_document: $ => seq(
      alias($._text_lang_line, $.lang_line),
      repeat(choice($.text, $.at_expression, $.text_comment)),
      $._text_close,
    ),

    // Group sequences
    // ===============

    // Groups separated by newlines (at the sequence's column) or `;`
    _groups: $ => seq(
      optional($._semis),
      $._group_item,
      repeat(seq($._sep, $._group_item)),
      optional($._semis),
    ),

    _group_item: $ => choice($.group, $.group_comment, $.splice),

    // `#//` comments out the group that follows it
    group_comment: $ => seq('#//', $.group),

    // `;«...»` splices a line- and column-insensitive group sequence into the
    // enclosing one
    splice: $ => seq(
      alias(/;[ \t]*«/, ';«'),
      optional(seq($._splice_start, $._groups, $._layout_end)),
      '»',
    ),

    _semis: _ => repeat1(';'),

    _sep: $ => choice(
      seq($._layout_sep, optional($._semis)),
      seq($._semis, optional($._layout_sep)),
    ),

    // Groups inside `()`, `[]` and `{}`, separated by `,`
    _comma_groups: $ => seq(
      $._group_item,
      repeat(seq(',', $._group_item)),
      optional(','),
    ),

    group: $ => choice(
      seq(repeat1($._term), optional($.block), optional($.alternatives)),
      // only valid at the start of a group sequence inside a pair
      seq($.block, optional($.alternatives)),
      $.alternatives,
    ),

    // Blocks and alternatives
    // -----------------------

    block: $ => seq(':', optional($._block_body)),

    // a following `#// | ...` stays with these alternatives
    alternatives: $ => prec.right(repeat1(choice($.alternative, $.alternative_comment))),

    // `#// | ...` comments out one alternative rather than a group of them
    alternative_comment: $ => prec(1, seq('#//', $.alternative)),

    alternative: $ => seq('|', optional($._alt_body)),

    _block_body: $ => choice(
      seq($._block_start, $._groups, $._layout_end),
      $._guillemet_groups,
    ),

    _alt_body: $ => choice(
      seq($._alt_start, $._groups, $._layout_end),
      $._guillemet_groups,
    ),

    _guillemet_groups: $ => seq(
      '«',
      optional(seq($._guillemet_start, $._groups, $._layout_end)),
      '»',
    ),

    // Terms
    // =====

    _term: $ => choice(
      $.identifier,
      $.keyword,
      $.operator,
      $.number,
      $.string,
      $.byte_string,
      $.boolean,
      $.void,
      $.sexp_escape,
      $.parens,
      $.brackets,
      $.braces,
      $.quotes,
      $.at_expression,
    ),

    parens: $ => seq('(', optional($._bracket_body), ')'),
    brackets: $ => seq('[', optional($._bracket_body), ']'),
    braces: $ => seq('{', optional($._bracket_body), '}'),

    _bracket_body: $ => seq($._bracket_start, $._comma_groups, $._layout_end),

    // The quote context is always opened, even for `''`, so that the scanner
    // can tell a closing `'` from an opening one
    quotes: $ => seq(
      '\'',
      $._quote_start,
      optional(choice($._groups, $._guillemet_groups)),
      $._layout_end,
      '\'',
    ),

    // At-notation
    // -----------

    // `@command(arg, ...){text}...`, where each part is optional. The scanner
    // enforces that no space separates the parts.
    at_expression: $ => prec.right(seq(
      choice('@', alias($._text_escape, '@')),
      choice(
        $._at_splice,
        seq(
          field('command', $._at_command),
          optional(field('arguments', $.at_arguments)),
          repeat(field('text', $.text_block)),
        ),
        repeat1(field('text', $.text_block)),
      ),
    )),

    // `@(«group»)` splices the group as-is
    _at_splice: $ => seq(
      alias(token.immediate(/\(\s*«/), '(«'),
      optional(seq($._splice_start, $._groups, $._layout_end)),
      alias(/»\s*\)/, '»)'),
    ),

    _at_command: $ => choice(
      $.identifier,
      $.keyword,
      $.operator,
      $.number,
      $.string,
      $.byte_string,
      $.boolean,
      $.void,
      $.sexp_escape,
      $.parens,
      $.brackets,
      $.quotes,
      $.at_command,
    ),

    // A multi-part command: `«term ...»`, or `a.b.c` with no spaces
    at_command: $ => choice(
      seq('«', optional(seq($._splice_start, $._groups, $._layout_end)), '»'),
      seq(
        $.identifier,
        repeat1(seq(
          alias($._at_command_operator, $.operator),
          alias(token.immediate(/(#%)?[\p{L}_][\p{L}\p{N}_]*/), $.identifier),
        )),
      ),
    ),

    // Arguments may be separated by newlines instead of `,`
    at_arguments: $ => seq(
      alias($._at_args_open, '('),
      optional(seq(
        $._bracket_start,
        $._group_item,
        repeat(seq(choice(',', $._layout_sep), $._group_item)),
        optional(','),
        $._layout_end,
      )),
      ')',
    ),

    // `{...}` or `|<<{...}>>|`, with `@` (or `|<<@`) escapes
    text_block: $ => seq(
      $._text_open,
      repeat(choice($.text, $.at_expression, $.text_comment)),
      $._text_close,
    ),

    // Atoms
    // -----

    identifier: _ => /(#%)?[\p{L}_][\p{L}\p{N}_]*/,

    keyword: _ => /~[\p{L}_][\p{L}\p{N}_]*/,

    // The scanner decides when a leading sign belongs to the number
    number: _ => token(choice(
      /[+-]?0x[0-9a-fA-F_]*[0-9a-fA-F]/,
      /[+-]?0o[0-7_]*[0-7]/,
      /[+-]?0b[01_]*[01]/,
      /[+-]?\d[\d_]*(\.\d[\d_]*)?([eE][+-]?\d+)?/,
      /[+-]?\.\d[\d_]*([eE][+-]?\d+)?/,
      // a fraction's denominator must be nonzero: `1/0` is `1 / 0`
      /[+-]?\d[\d_]*\/[\d_]*[1-9][\d_]*/,
      '#inf',
      '#neginf',
      '#nan',
    )),

    string: _ => /"([^"\\\n\r]|\\.)*"/,

    byte_string: _ => /#"([^"\\\n\r]|\\.)*"/,

    boolean: _ => choice('#true', '#false'),

    void: _ => '#void',

    // `#{...}` and `~#{...}` escape to Racket S-expression syntax; nested
    // braces are not balanced yet
    sexp_escape: _ => /~?#\{[^}]*\}/,

    comment: _ => token(choice(
      /\/\/[^\n]*/,
      /\/\*[^*]*\*+([^/*][^*]*\*+)*\//,
      /#![^\n]*/,
      /@\/\/[^\n]*/,
    )),

    lang_line: _ => /#lang[^\n]*/,
  },
});
