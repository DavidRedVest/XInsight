; Self-authored identifier-occurrence query for the C++ grammar (PRD 3.2/5.3).
; See query/c/references.scm's header for the rationale -- same approach,
; plus namespace_identifier for C++'s `ns::name` qualifiers.
;
; Node types verified against tree-sitter-cpp v0.23.4's src/node-types.json.
; qualified_identifier itself is deliberately not captured: it's a composite
; node (scope + name), and the identifier/namespace_identifier leaves inside
; it are already matched individually by the patterns below.

(identifier) @reference
(field_identifier) @reference
(type_identifier) @reference
(statement_identifier) @reference
(namespace_identifier) @reference
