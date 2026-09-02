#!/bin/sh
set -e

# Normalize mtimes of the pre-generated parser sources so CMake's
# custom-command dependency check doesn't consider them stale and try to
# shell out to the tree-sitter CLI to regenerate them. Keeps the build
# self-contained and reproducible regardless of what's on the host.
find . \( -name '*.c' -o -name '*.h' -o -name '*.json' -o -name 'grammar.js' \) -exec touch {} +

# tree-sitter-c and tree-sitter-cpp both declare add_custom_target(ts-test
# ...), which collides when both grammars are add_subdirectory'd into the
# same CMake project. We don't need it (it shells out to the tree-sitter CLI
# to run the grammar's own test suite), so drop it.
if [ -f CMakeLists.txt ]; then
  sed -i '' '/add_custom_target(ts-test/,/COMMENT "tree-sitter test")/d' CMakeLists.txt
fi
