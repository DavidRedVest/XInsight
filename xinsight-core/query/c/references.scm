; Self-authored identifier-occurrence query for the C grammar (PRD 3.2/5.3:
; "查引用...默认走 tree-sitter 的标识符出现索引(模糊,基于名字)"). Every
; identifier-like leaf token becomes a reference candidate -- both
; definition sites and use sites, deliberately not scope-aware -- but only
; real identifier tokens: tree-sitter's grammar never lexes comment or
; string/char-literal contents as identifier nodes, so those are excluded
; structurally rather than by any filtering here.
;
; Node types verified against tree-sitter-c v0.24.2's src/node-types.json,
; not guessed.

(identifier) @reference
(field_identifier) @reference
(type_identifier) @reference
(statement_identifier) @reference
