; Adapted from nvim-treesitter's queries/cpp/highlights.scm (master branch,
; commit 835f5c11b8d4e1ded3576c69a019f717d3754c5a), licensed Apache-2.0.
; See /THIRD_PARTY_LICENSES for the full license text and attribution.
;
; Applied ON TOP OF query/c/highlights.scm for .cpp/.hpp files (TreeSitterEngine
; runs the C query first, then this one, and later matches win on overlapping
; byte ranges) -- mirrors upstream's own "; inherits: c" relationship without
; needing Neovim's query-composition system.
;
; Same predicate-support trims as query/c/highlights.scm apply here (see that
; file's header comment). Additionally, deep (3+ level) namespace-qualified
; function/call detection and the `#has-ancestor?`-gated fallback for it were
; dropped -- capped at two qualification levels (Class::method,
; ns::Class::method), which covers the embedded/BSP-style C++ this tool
; targets; deeper qualification just falls back to plain @variable/@type
; coloring instead of @function, a cosmetic-only gap.

((identifier) @variable.member
  (#match? @variable.member "^m_.*$"))

(parameter_declaration
  declarator: (reference_declarator) @variable.parameter)

(variadic_parameter_declaration
  declarator: (variadic_declarator
    (_) @variable.parameter))

(optional_parameter_declaration
  declarator: (_) @variable.parameter)

((field_expression
  (field_identifier) @function.method) @_parent
  (#has-parent? @_parent template_method function_declarator))

(field_declaration
  (field_identifier) @variable.member)

(field_initializer
  (field_identifier) @property)

(function_declarator
  declarator: (field_identifier) @function.method)

(concept_definition
  name: (identifier) @type.definition)

(alias_declaration
  name: (type_identifier) @type.definition)

(auto) @type.builtin

(namespace_identifier) @module

(using_declaration
  .
  "using"
  .
  "namespace"
  .
  [
    (qualified_identifier)
    (identifier)
  ] @module)

(destructor_name
  (identifier) @function.method)

; Out-of-line function/method names, up to two qualification levels.
(function_declarator
  (qualified_identifier
    (identifier) @function))

(function_declarator
  (qualified_identifier
    (qualified_identifier
      (identifier) @function)))

(function_declarator
  (template_function
    (identifier) @function))

(operator_name) @function

"operator" @function

"static_assert" @function.builtin

(call_expression
  (qualified_identifier
    (identifier) @function.call))

(call_expression
  (qualified_identifier
    (qualified_identifier
      (identifier) @function.call)))

(call_expression
  (template_function
    (identifier) @function.call))

(call_expression
  (field_expression
    (field_identifier) @function.method.call))

; Constructors: heuristic on a capitalized name, same convention as upstream.
((function_declarator
  (qualified_identifier
    (identifier) @constructor))
  (#match? @constructor "^[A-Z]"))

((call_expression
  function: (identifier) @constructor)
  (#match? @constructor "^[A-Z]"))

((call_expression
  function: (qualified_identifier
    name: (identifier) @constructor))
  (#match? @constructor "^[A-Z]"))

; Constants
(this) @variable.builtin

(null
  "nullptr" @constant.builtin)

(true) @boolean

(false) @boolean

; Literals
(raw_string_literal) @string

; Keywords
[
  "try"
  "catch"
  "noexcept"
  "throw"
] @keyword.exception

[
  "decltype"
  "explicit"
  "friend"
  "override"
  "using"
  "requires"
  "constexpr"
] @keyword

[
  "class"
  "namespace"
  "template"
  "typename"
  "concept"
] @keyword.type

[
  "co_await"
  "co_yield"
  "co_return"
] @keyword.coroutine

[
  "public"
  "private"
  "protected"
  "final"
  "virtual"
] @keyword.modifier

[
  "new"
  "delete"
  "xor"
  "bitand"
  "bitor"
  "compl"
  "not"
  "xor_eq"
  "and_eq"
  "or_eq"
  "not_eq"
  "and"
  "or"
] @keyword.operator

"<=>" @operator

"::" @punctuation.delimiter

(template_argument_list
  [
    "<"
    ">"
  ] @punctuation.bracket)

(template_parameter_list
  [
    "<"
    ">"
  ] @punctuation.bracket)

(literal_suffix) @operator
