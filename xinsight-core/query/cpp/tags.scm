; Self-authored definition-extraction query for the C++ grammar (PRD 8.1).
; Field names verified against tree-sitter-cpp v0.23.4's src/node-types.json.
;
; Deliberately not exhaustive: operator overloads, templated class/function
; names, deeply (3+) qualified out-of-line definitions, and nested namespace
; specifiers (`namespace a::b {}`) are known gaps -- "tolerant, not
; IDE-precise", per PRD.

; Free functions and inline methods (pointer-returning variants included).
(function_definition
  declarator: (function_declarator
    declarator: (identifier) @name)) @definition.function

(function_definition
  declarator: (pointer_declarator
    declarator: (function_declarator
      declarator: (identifier) @name))) @definition.function

(function_definition
  declarator: (function_declarator
    declarator: (field_identifier) @name)) @definition.method

; Out-of-line method definitions: Foo::bar(...) / ns::Foo::bar(...).
(function_definition
  declarator: (function_declarator
    declarator: (qualified_identifier
      name: (identifier) @name))) @definition.method

(function_definition
  declarator: (function_declarator
    declarator: (qualified_identifier
      name: (qualified_identifier
        name: (identifier) @name)))) @definition.method

; Constructors/destructors defined out-of-line.
(function_definition
  declarator: (function_declarator
    declarator: (qualified_identifier
      name: (destructor_name) @name))) @definition.method

; class/struct: only a definition when a body is present.
(class_specifier
  name: (type_identifier) @name
  body: (field_declaration_list)) @definition.class

(struct_specifier
  name: (type_identifier) @name
  body: (field_declaration_list)) @definition.struct

(union_specifier
  name: (type_identifier) @name
  body: (field_declaration_list)) @definition.union

(enum_specifier
  name: (type_identifier) @name
  body: (enumerator_list)) @definition.enum

(namespace_definition
  name: (namespace_identifier) @name) @definition.namespace

; typedef / using-alias.
(type_definition
  declarator: (type_identifier) @name) @definition.typedef

(alias_declaration
  name: (type_identifier) @name) @definition.typedef

; macros.
(preproc_def
  name: (identifier) @name) @definition.macro

(preproc_function_def
  name: (identifier) @name) @definition.macro

; global variables: direct children of the translation unit or of a
; (non-nested) namespace body.
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

(namespace_definition
  body: (declaration_list
    (declaration
      declarator: (identifier) @name) @definition.variable))

(namespace_definition
  body: (declaration_list
    (declaration
      declarator: (init_declarator
        declarator: (identifier) @name)) @definition.variable))
