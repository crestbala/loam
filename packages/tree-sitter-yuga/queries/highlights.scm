; Syntax highlighting for Yuga (Zed + tree-sitter)

(comment) @comment

[
  "fn"
  "async"
  "await"
  "let"
  "const"
  "mut"
  "struct"
  "enum"
  "import"
  "if"
  "else"
  "for"
  "while"
  "in"
  "return"
  "break"
  "continue"
  "match"
  "as"
] @keyword

[
  "true"
  "false"
] @boolean

(number) @number
(string) @string
(interpolation) @embedded
(identifier) @variable

(function_item
  name: (identifier) @function)

(parameter
  name: (identifier) @variable.parameter)

(let_statement
  name: (identifier) @variable)

(const_item
  name: (identifier) @constant)

(field_declaration
  name: (identifier) @property)

(field_initializer
  name: (identifier) @property)

(named_argument
  name: (identifier) @property)

(enum_item
  name: (identifier) @type)

(enum_variant
  name: (identifier) @constant)

(call_expression
  function: (field_expression
    value: (identifier) @namespace
    field: (identifier) @function))

(field_expression
  field: (identifier) @property)

((identifier) @type
 (#match? @type "^[A-Z]"))

(call_expression
  function: (identifier) @function)

(call_expression
  function: (path_expression
    name: (identifier) @function))

; Primitive scalar types are data types, not functions — `u8(x)` is a
; conversion. After the call_expression rules so it wins over `@function`.
((identifier) @type.builtin
 (#match? @type.builtin "^(i8|i16|i32|i64|u8|u16|u32|u64|f32|f64|int|float|bool|string)$"))

(struct_item
  name: (identifier) @type)

(struct_literal
  type: (identifier) @type)

(cast_expression
  type: (_) @type)

(generic_type
  (identifier) @type)

(fn_type) @type
(reference_type) @type
(box_type) @type
(array_type) @type
"Box" @type.builtin

[
  "&"
  "*"
  "!"
  "-"
  "+"
  "/"
  "%"
  "="
  "+="
  "-="
  "*="
  "/="
  "=="
  "!="
  "<"
  ">"
  "<="
  ">="
  "&&"
  "||"
  ".."
  "->"
  "=>"
  "::"
  "|"
] @operator

[
  "("
  ")"
  "{"
  "}"
  "["
  "]"
] @punctuation.bracket

[
  ","
  ":"
  ";"
  "."
] @punctuation.delimiter
