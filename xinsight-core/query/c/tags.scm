; Self-authored definition-extraction query for the C grammar (PRD 8.1: we
; own "what counts as a definition" since it directly determines symbol
; index quality). Field names verified against tree-sitter-c v0.24.2's
; src/node-types.json, not guessed.
;
; Deliberately not exhaustive: function-pointer typedefs, K&R-style
; parameter lists, and triple-pointer declarators are known gaps. Good
; enough for "read code", per PRD's "tolerant, not IDE-precise" stance.

; Functions: plain and single/double pointer-returning.
(function_definition
  declarator: (function_declarator
    declarator: (identifier) @name)) @definition.function

(function_definition
  declarator: (pointer_declarator
    declarator: (function_declarator
      declarator: (identifier) @name))) @definition.function

(function_definition
  declarator: (pointer_declarator
    declarator: (pointer_declarator
      declarator: (function_declarator
        declarator: (identifier) @name)))) @definition.function

; struct/enum/union: only count as a definition when a body is present, so
; a mere reference like `struct S *p;` doesn't get indexed as a definition.
(struct_specifier
  name: (type_identifier) @name
  body: (field_declaration_list)) @definition.struct

(enum_specifier
  name: (type_identifier) @name
  body: (enumerator_list)) @definition.enum

(union_specifier
  name: (type_identifier) @name
  body: (field_declaration_list)) @definition.union

; typedef (simple named-type and typedef-struct forms).
(type_definition
  declarator: (type_identifier) @name) @definition.typedef

; macros (object-like and function-like).
(preproc_def
  name: (identifier) @name) @definition.macro

(preproc_function_def
  name: (identifier) @name) @definition.macro

; global variables: declarations that are direct children of the
; translation unit (i.e. not local to a function body).
(translation_unit
  (declaration
    declarator: (identifier) @name) @definition.variable)

(translation_unit
  (declaration
    declarator: (init_declarator
      declarator: (identifier) @name)) @definition.variable)

(translation_unit
  (declaration
    declarator: (pointer_declarator
      declarator: (identifier) @name)) @definition.variable)

(translation_unit
  (declaration
    declarator: (init_declarator
      declarator: (pointer_declarator
        declarator: (identifier) @name))) @definition.variable)
