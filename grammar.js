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
    $._error_sentinel,
  ],

  extras: $ => [/\s/, $.comment, $.lang_line, /\\/, $._newline_mark],

  rules: {
    source_file: $ => optional(seq($._block_start, $._groups, $._layout_end)),

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
      $.at,
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

    // Placeholder until at-notation is parsed properly
    at: _ => '@',

    comment: _ => token(choice(
      /\/\/[^\n]*/,
      /\/\*[^*]*\*+([^/*][^*]*\*+)*\//,
      /#![^\n]*/,
    )),

    lang_line: _ => /#lang[^\n]*/,
  },
});
