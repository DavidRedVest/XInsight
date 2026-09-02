; Self-authored fold-range query for the C++ grammar (PRD 8.1).
; (compound_statement) covers function/method/if/for/while bodies.

(class_specifier
  body: (field_declaration_list) @fold)

(struct_specifier
  body: (field_declaration_list) @fold)

(union_specifier
  body: (field_declaration_list) @fold)

(enum_specifier
  body: (enumerator_list) @fold)

(namespace_definition
  body: (declaration_list) @fold)

(compound_statement) @fold

(initializer_list) @fold

(comment) @fold

(preproc_ifdef) @fold
