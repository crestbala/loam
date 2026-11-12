module.exports = grammar({
  name: 'loam',

  extras: $ => [
    /\s/,
    $.doc_comment,
    $.comment,
  ],

  // See src/scanner.c. `_error_sentinel` is never produced: it is only valid
  // during error recovery, which is how the scanner knows to stand aside.
  externals: $ => [$._line_break, $._error_sentinel],

  word: $ => $.identifier,

  conflicts: $ => [
    [$._expr_no_struct, $.generic_struct_name],
    // `(a, b)` is a tuple or the head of `(a, b) => ...` until the `=>`.
    [$._expr_no_struct, $.closure],
  ],

  rules: {
    source_file: $ => repeat($._item),

    _item: $ => choice(
      $.import_item,
      $.file_attribute,
      $.function_item,
      $.struct_item,
      $.enum_item,
      $.const_item,
    ),

    import_item: $ => seq(
      'import',
      choice(
        seq(
          choice($.identifier, $.string),
          optional(seq('as', field('alias', $.identifier))),
        ),
        // Selective: `import { A, B as C } "path"`. No `from`, no `export`.
        seq(
          '{',
          optional(seq($.import_name, repeat(seq(',', $.import_name)))),
          '}',
          field('path', $.string),
        ),
      ),
      optional(';'),
    ),

    import_name: $ => seq(
      field('name', $.identifier),
      optional(seq('as', field('local', $.identifier))),
    ),

    attribute: $ => seq('#', '[', $.identifier, optional($._attribute_args), ']'),

    /* `#![c_source("x.c")]` / `#![c_link("-lm")]`: file-level, no item follows. */
    file_attribute: $ => seq('#', '!', '[', $.identifier, optional($._attribute_args), ']'),

    _attribute_args: $ => seq('(', $.string, repeat(seq(',', $.string)), optional(','), ')'),

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

    /* A named `fn` inside a block. No `async` or attribute here, so `async`
       stays an ordinary name at statement start (`async.tick()`). Closures
       have no name, so the two never meet. */
    local_function: $ => seq(
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
      // Fields end at a comma or a newline (`frm: []int` then `to: []int`).
      optional(seq($.field_declaration, repeat(seq(optional(','), $.field_declaration)), optional(','))),
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
      choice(
        seq(optional('mut'), field('name', $.identifier), optional(seq(':', $._type))),
        seq(field('names', $.let_tuple), optional(seq(':', $.let_tuple_types))),
      ),
      '=',
      field('value', $._expression),
      optional($._stmt_end),
    ),

    _stmt_end: $ => choice(';', $._line_break),

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
      $.local_function,
      $.let_statement,
      $.if_statement,
      $.for_statement,
      $.while_statement,
      $.switch_statement,
      $.return_statement,
      $.break_statement,
      $.continue_statement,
      $.assignment_statement,
      $.ui_call_statement,
      $.expression_statement,
    ),

    let_statement: $ => seq(
      choice('let', 'const'),
      choice(
        seq(optional('mut'), field('name', $.identifier), optional(seq(':', $._type))),
        seq(field('names', $.let_tuple), optional(seq(':', $.let_tuple_types))),
      ),
      '=',
      field('value', $._expression),
      optional($._stmt_end),
    ),

    // `let (a, mut b): (int, f64) = (1, 2.0)`
    let_tuple: $ => seq(
      '(',
      $._let_tuple_name,
      repeat(seq(',', $._let_tuple_name)),
      optional(','),
      ')',
    ),

    _let_tuple_name: $ => seq(optional('mut'), field('name', $.identifier)),

    let_tuple_types: $ => seq('(', $._type, repeat(seq(',', $._type)), optional(','), ')'),

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

    /* `switch v { case A, B: ... default: ... }`: as in C, a case falls into
       the next one unless a `break` leaves the switch. */
    switch_statement: $ => seq(
      'switch',
      field('value', $._condition),
      '{',
      repeat($.switch_case),
      '}',
    ),

    switch_case: $ => seq(
      choice(
        seq('case', field('pattern', $._expression), repeat(seq(',', field('pattern', $._expression)))),
        'default',
      ),
      ':',
      repeat(seq($._statement, optional(','))),
    ),

    // A bare `return` takes no `_line_break`: like the compiler, it reads a
    // value from the next line, so `return` then `(x)` returns `x`.
    return_statement: $ => prec.right(choice(
      seq('return', $._expression, optional($._stmt_end)),
      seq('return', optional(';')),
    )),

    break_statement: $ => seq('break', optional(';')),
    continue_statement: $ => seq('continue', optional(';')),

    ui_call_statement: $ => seq(
      $.call_expression,
      $.block,
      optional(';'),
    ),

    /* `Box(...) { ... }` in argument position: the trailing block is the slot. */
    ui_call_expression: $ => prec(13, seq(
      $.call_expression,
      $.block,
    )),

    assignment_statement: $ => seq(
      $._expression,
      choice('=', '+=', '-=', '*=', '/=', '%=',
             '&=', '|=', '^=', '<<=', '>>='),
      $._expression,
      optional($._stmt_end),
    ),

    expression_statement: $ => seq($._expr_no_if, optional($._stmt_end)),

    _condition: $ => $._expr_no_struct,

    _expr_no_if: $ => choice(
      $.struct_literal,
      $.struct_method_call,
      $.ui_call_expression,
      $._expr_no_struct,
    ),

    _expression: $ => choice(
      $.if_expression,
      $._expr_no_if,
    ),

    _expr_no_struct: $ => choice(
      $.binary_expression,
      $.unary_expression,
      $.update_expression,
      $.call_expression,
      $.index_expression,
      $.field_expression,
      $.path_expression,
      $.cast_expression,
      $.try_expression,
      $.closure,
      $.array_literal,
      $.parenthesized_expression,
      $.tuple_expression,
      $.identifier,
      $.number,
      $.string,
      $.boolean,
    ),

    parenthesized_expression: $ => seq('(', $._expression, ')'),

    tuple_expression: $ => seq(
      '(', $._expression, ',', $._expression, repeat(seq(',', $._expression)), optional(','), ')',
    ),

    closure: $ => choice(
      seq('||', optional(seq('->', $._type)), $.block),
      seq(
        '|',
        optional(seq($.parameter, repeat(seq(',', $.parameter)), optional(','))),
        '|',
        optional(seq('->', $._type)),
        $.block,
      ),
      seq(
        '(',
        optional(seq($.identifier, repeat(seq(',', $.identifier)), optional(','))),
        ')',
        '=>',
        choice($.block, prec(14, $._expr_no_struct)),
      ),
      seq(
        'fn',
        $.parameters,
        optional(seq('->', $._type)),
        choice($.block, seq('=>', choice($.block, $._expr_no_struct))),
      ),
    ),

    // `i++` / `++i`: values, not only statements (`let p = a++`).
    update_expression: $ => choice(
      prec(13, seq(field('value', $._expr_no_struct), choice('++', '--'))),
      prec(12, seq(choice('++', '--'), field('value', $._expr_no_struct))),
    ),

    unary_expression: $ => prec(12, choice(
      seq('&', optional('mut'), $._expr_no_struct),
      seq('*', $._expr_no_struct),
      seq('!', $._expr_no_struct),
      seq('~', $._expr_no_struct),
      seq('-', $._expr_no_struct),
      seq('await', $._expr_no_struct),
    )),

    binary_expression: $ => {
      const ops = [
        [1, '..'],
        [2, '||'],
        [3, '&&'],
        [4, '|'],
        [5, '^'],
        [6, '&'],
        [7, choice('==', '!=')],
        [8, choice('<', '>', '<=', '>=')],
        [9, choice('<<', '>>')],
        [10, choice('+', '-')],
        [11, choice('*', '/', '%')],
      ];
      return choice(...ops.map(([p, op]) =>
        prec.left(p, seq(
          field('left', $._expr_no_struct),
          field('operator', op),
          // `r.w + if r.all { tail } else { 0 }`: an `if` value as the right
          // operand. It ends at its `else` block, so it takes nothing after it.
          field('right', choice($._expr_no_struct, $.if_expression)),
        ))
      ));
    },

    call_expression: $ => prec(13, seq(
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
      prec(15, $.named_argument),
      $._expression,
    ),

    field_expression: $ => prec(13, seq(
      field('value', $._expr_no_struct),
      '.',
      field('field', $.identifier),
    )),

    path_expression: $ => prec(13, seq(
      field('value', $._expr_no_struct),
      '::',
      field('name', $.identifier),
    )),

    // Postfix cast: `n as float`. Must appear in the grammar so Zed's
    // highlights.scm can match the anonymous node `"as"`.
    cast_expression: $ => prec(13, seq(
      field('value', $._expr_no_struct),
      'as',
      field('type', $._type),
    )),

    // `f(x)?`: unwrap a Result, or return its error from the enclosing fn.
    try_expression: $ => prec(13, seq(
      field('value', $._expr_no_struct),
      '?',
    )),

    index_expression: $ => prec(13, seq(
      field('value', $._expr_no_struct),
      '[',
      field('index', $._expression),
      ']',
    )),

    struct_literal: $ => prec.dynamic(-1, seq(
      field('type', choice($.identifier, $.generic_struct_name)),
      '{',
      optional(seq($.field_initializer, repeat(seq(',', $.field_initializer)), optional(','))),
      '}',
    )),

    /* `Signal<int> { id: x }.set(5)`: a method on a struct literal. Only where
       a struct literal may stand, never in a condition, where `if x {` must
       stay a block. */
    struct_method_call: $ => prec(13, seq(
      field('value', $.struct_literal),
      '.',
      field('method', $.identifier),
      field('arguments', $.arguments),
    )),

    /* `Map<T>` heading a struct literal. `a < b` reads the same way up to the
       `>`, so this is a GLR conflict, settled by what follows: only a `{`
       makes it a type. */
    generic_struct_name: $ => prec.dynamic(-2, seq(
      field('name', $.identifier),
      '<',
      $._type,
      repeat(seq(',', $._type)),
      '>',
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

    // A character literal (`'a'`, `'\n'`, `'\x7f'`) is an integer literal.
    number: _ => token(choice(
      /0[xX][0-9a-fA-F]+/,
      /[0-9]+(\.[0-9]+)?([eE][+-]?[0-9]+)?/,
      /'([^'\\\n]|\\x[0-9a-fA-F]{2}|\\[^x\n])'/,
    )),

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

    // `{{ expr }}`; the expression may hold one level of `{ ... }`, as in
    // `{{key.name(Tonality { tonic: t })}}`.
    interpolation: _ => seq('{{', repeat1(choice(/[^{}]+/, /\{[^{}]*\}/)), '}}'),

    // Doc comment `/** *text */`. Higher precedence than `comment`, which
    // also matches this span. A non-star must follow the opener, so a run
    // of three stars stays a comment. A bang block is an ordinary comment.
    doc_comment: _ => token(prec(1,
      /\/\*\*[^*][^*]*\*+([^/*][^*]*\*+)*\//,
    )),

    comment: _ => token(choice(
      seq('//', /[^\n]*/),
      seq('/*', /[^*]*\*+([^/*][^*]*\*+)*/, '/'),
    )),
  },
});
