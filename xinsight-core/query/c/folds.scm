; Self-authored fold-range query for the C grammar (PRD 8.1).
; (compound_statement) below already covers function/if/for/while bodies.

(struct_specifier
  body: (field_declaration_list) @fold)

(enum_specifier
  body: (enumerator_list) @fold)

(union_specifier
  body: (field_declaration_list) @fold)

(compound_statement) @fold

(initializer_list) @fold

(comment) @fold

(preproc_ifdef) @fold
