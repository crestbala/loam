module.exports = grammar({
  name: 'yuga',

  extras: $ => [
    /\s/,
    $.comment,
  ],

  word: $ => $.identifier,

  rules: {
    source_file: $ => repeat($._item),

    _item: $ => choice(
      $.import_item,
      $.function_item,
      $.struct_item,
      $.enum_item,
      $.const_item,
    ),

    import_item: $ => seq(
      'import',
      choice($.identifier, $.string),
      optional(';'),
    ),

    attribute: $ => seq('#', '[', $.identifier, ']'),

    function_item: $ => seq(
      optional($.attribute),
      optional(field('async', 'async')),
      'fn',
      field('name', $.identifier),
      optional($.type_params),
      field('parameters', $.fn_parameters),
      optional(seq('->', field('return_type', $._type))),
      field('body', $.block),
    ),

    type_params: $ => seq(
      '<',
      $.identifier,
      repeat(seq(',', $.identifier)),
      optional(','),
      '>',
    ),

    parameters: $ => seq(
      '(',
      optional(seq($.parameter, repeat(seq(',', $.parameter)), optional(','))),
      ')',
    ),

    fn_parameters: $ => seq(
      '(',
      optional(seq($.defaulted_parameter, repeat(seq(',', $.defaulted_parameter)), optional(','))),
      ')',
    ),

    defaulted_parameter: $ => seq(
      $.parameter,
      optional(seq('=', field('default', $._expression))),
    ),

    parameter: $ => seq(
      field('name', $.identifier),
      optional(seq(':', field('type', $._type))),
    ),

    struct_item: $ => seq(
      optional($.attribute),
      'struct',
      field('name', $.identifier),
      optional($.type_params),
      '{',
      optional(seq($.field_declaration, repeat(seq(',', $.field_declaration)), optional(','))),
      '}',
    ),

    field_declaration: $ => seq(
      field('name', $.identifier),
      ':',
      field('type', $._type),
      optional(seq('=', $._expression)),
    ),

    enum_item: $ => seq(
      'enum',
      field('name', $.identifier),
      '{',
      optional(seq($.enum_variant, repeat(seq(',', $.enum_variant)), optional(','))),
      '}',
    ),

    enum_variant: $ => seq(
      field('name', $.identifier),
      optional(seq('=', optional('-'), $.number)),
    ),

    const_item: $ => seq(
      choice('const', 'let'),
      optional('mut'),
      field('name', $.identifier),
      optional(seq(':', $._type)),
      '=',
      field('value', $._expression),
      optional(';'),
    ),

    _type: $ => choice(
      $.reference_type,
      $.box_type,
      $.array_type,
      $.fn_type,
      $.generic_type,
      $.identifier,
    ),

    reference_type: $ => seq('&', optional('mut'), $._type),

    box_type: $ => seq('Box', '<', $._type, '>'),

    array_type: $ => choice(
      seq('[', $.number, ']', $._type),
      seq('[', ']', $._type),
    ),

    generic_type: $ => prec(2, seq(
      $.identifier,
      '<',
      $._type,
      repeat(seq(',', $._type)),
      optional(','),
      '>',
    )),

    fn_type: $ => seq(
      'fn',
      '(',
      optional(seq($._type, repeat(seq(',', $._type)), optional(','))),
      ')',
      optional(seq('->', $._type)),
    ),

    block: $ => seq('{', repeat(seq($._statement, optional(','))), '}'),

    _statement: $ => choice(
      $.let_statement,
      $.if_statement,
      $.for_statement,
      $.while_statement,
      $.match_statement,
      $.return_statement,
      $.break_statement,
      $.continue_statement,
      $.assignment_statement,
      $.ui_call_statement,
      $.expression_statement,
    ),

    let_statement: $ => seq(
      choice('let', 'const'),
      optional('mut'),
      field('name', $.identifier),
      optional(seq(':', $._type)),
      '=',
      field('value', $._expression),
      optional(';'),
    ),

    if_statement: $ => prec.right(seq(
      'if',
      field('condition', $._condition),
      field('consequence', $.block),
      optional(seq('else', field('alternative', choice($.if_statement, $.block)))),
    )),

    /* `if cond { a } else { b }` as a value (e.g. a widget argument). */
    if_expression: $ => prec.right(seq(
      'if',
      field('condition', $._condition),
      field('consequence', $.block),
      'else',
      field('alternative', choice($.if_expression, $.block)),
    )),

    for_statement: $ => seq(
      'for',
      field('variable', $.identifier),
      'in',
      field('iterable', $._condition),
      field('body', $.block),
    ),

    while_statement: $ => seq(
      'while',
      field('condition', $._condition),
      field('body', $.block),
    ),

    match_statement: $ => seq(
      'match',
      field('value', $._condition),
      '{',
      repeat($.match_arm),
      '}',
    ),

    match_arm: $ => seq(
      $._expression,
      '=>',
      choice($.block, $._expression),
      optional(','),
    ),

    return_statement: $ => prec.right(seq(
      'return',
      optional($._expression),
      optional(';'),
    )),

    break_statement: $ => seq('break', optional(';')),
    continue_statement: $ => seq('continue', optional(';')),

    ui_call_statement: $ => seq(
      $.call_expression,
      $.block,
      optional(';'),
    ),

    assignment_statement: $ => seq(
      $._expression,
      choice('=', '+=', '-=', '*=', '/='),
      $._expression,
      optional(';'),
    ),

    expression_statement: $ => seq($._expr_no_if, optional(';')),

    _condition: $ => $._expr_no_struct,

    _expr_no_if: $ => choice(
      $.struct_literal,
      $._expr_no_struct,
    ),

    _expression: $ => choice(
      $.if_expression,
      $._expr_no_if,
    ),

    _expr_no_struct: $ => choice(
      $.binary_expression,
      $.unary_expression,
      $.call_expression,
      $.index_expression,
      $.field_expression,
      $.path_expression,
      $.cast_expression,
      $.closure,
      $.array_literal,
      $.parenthesized_expression,
      $.identifier,
      $.number,
      $.string,
      $.boolean,
    ),

    parenthesized_expression: $ => seq('(', $._expression, ')'),

    closure: $ => choice(
      seq('||', optional(seq('->', $._type)), $.block),
      seq(
        '|',
        optional(seq($.parameter, repeat(seq(',', $.parameter)), optional(','))),
        '|',
        optional(seq('->', $._type)),
        $.block,
      ),
      prec(9, seq(
        '(',
        optional(seq($.identifier, repeat(seq(',', $.identifier)), optional(','))),
        ')',
        '=>',
        $.block,
      )),
      seq('fn', $.parameters, optional(seq('->', $._type)), $.block),
    ),

    unary_expression: $ => prec(7, choice(
      seq('&', optional('mut'), $._expr_no_struct),
      seq('*', $._expr_no_struct),
      seq('!', $._expr_no_struct),
      seq('-', $._expr_no_struct),
      seq('await', $._expr_no_struct),
    )),

    binary_expression: $ => {
      const ops = [
        [1, '..'],
        [2, '||'],
        [3, '&&'],
        [4, choice('==', '!=', '<', '>', '<=', '>=')],
        [5, choice('+', '-')],
        [6, choice('*', '/', '%')],
      ];
      return choice(...ops.map(([p, op]) =>
        prec.left(p, seq(
          field('left', $._expr_no_struct),
          field('operator', op),
          field('right', $._expr_no_struct),
        ))
      ));
    },

    call_expression: $ => prec(8, seq(
      field('function', $._expr_no_struct),
      field('arguments', $.arguments),
    )),

    arguments: $ => seq(
      '(',
      optional(seq($._argument, repeat(seq(',', $._argument)), optional(','))),
      ')',
    ),

    named_argument: $ => seq(
      field('name', $.identifier),
      '=',
      field('value', $._expression),
    ),

    _argument: $ => choice(
      prec(10, $.named_argument),
      $._expression,
    ),

    field_expression: $ => prec(8, seq(
      field('value', $._expr_no_struct),
      '.',
      field('field', $.identifier),
    )),

    path_expression: $ => prec(8, seq(
      field('value', $._expr_no_struct),
      '::',
      field('name', $.identifier),
    )),

    // Postfix cast: `n as float`. Must appear in the grammar so Zed's
    // highlights.scm can match the anonymous node `"as"`.
    cast_expression: $ => prec(8, seq(
      field('value', $._expr_no_struct),
      'as',
      field('type', $._type),
    )),

    index_expression: $ => prec(8, seq(
      field('value', $._expr_no_struct),
      '[',
      field('index', $._expression),
      ']',
    )),

    struct_literal: $ => prec.dynamic(-1, seq(
      field('type', $.identifier),
      '{',
      optional(seq($.field_initializer, repeat(seq(',', $.field_initializer)), optional(','))),
      '}',
    )),

    field_initializer: $ => seq(
      field('name', $.identifier),
      ':',
      field('value', $._expression),
    ),

    array_literal: $ => seq(
      '[', optional($.number), ']', $._type,
      '{',
      optional(seq($._expression, repeat(seq(',', $._expression)), optional(','))),
      '}',
    ),

    boolean: _ => choice('true', 'false'),

    identifier: _ => /[a-zA-Z_][a-zA-Z0-9_]*/,

    number: _ => /[0-9]+(\.[0-9]+)?/,

    string: $ => seq(
      '"',
      repeat(choice(
        $.interpolation,
        token.immediate(prec(1, /[^"\\{]+/)),
        token.immediate(/\\./),
        token.immediate('{'),
      )),
      '"',
    ),

    interpolation: _ => seq('{{', /[^}]+/, '}}'),

    comment: _ => token(choice(
      seq('//', /[^\n]*/),
      seq('/*', /[^*]*\*+([^/*][^*]*\*+)*/, '/'),
    )),
  },
});
