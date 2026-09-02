include(FetchContent)

# tree-sitter runtime library
FetchContent_Declare(tree_sitter
  GIT_REPOSITORY https://github.com/tree-sitter/tree-sitter.git
  GIT_TAG v0.27.0
  GIT_SHALLOW TRUE)

# tree-sitter-c / tree-sitter-cpp ship src/parser.c pre-generated and committed
# to their release tags, but their CMakeLists.txt still declares a custom
# command that can regenerate it via the `tree-sitter` CLI, and both declare
# a colliding add_custom_target(ts-test ...) when built side by side. See
# cmake/patches/vendor-grammar-patch.sh for what this works around.
set(_xinsight_grammar_patch ${CMAKE_CURRENT_SOURCE_DIR}/cmake/patches/vendor-grammar-patch.sh)

FetchContent_Declare(tree_sitter_c
  GIT_REPOSITORY https://github.com/tree-sitter/tree-sitter-c.git
  GIT_TAG v0.24.2
  GIT_SHALLOW TRUE
  PATCH_COMMAND ${_xinsight_grammar_patch})

FetchContent_Declare(tree_sitter_cpp
  GIT_REPOSITORY https://github.com/tree-sitter/tree-sitter-cpp.git
  GIT_TAG v0.23.4
  GIT_SHALLOW TRUE
  PATCH_COMMAND ${_xinsight_grammar_patch})

set(JSON_BuildTests OFF CACHE BOOL "" FORCE)
set(JSON_Install OFF CACHE BOOL "" FORCE)
FetchContent_Declare(nlohmann_json
  GIT_REPOSITORY https://github.com/nlohmann/json.git
  GIT_TAG v3.12.0
  GIT_SHALLOW TRUE)

set(REPROC++ ON CACHE BOOL "" FORCE)
set(REPROC_TEST OFF CACHE BOOL "" FORCE)
set(REPROC_EXAMPLES OFF CACHE BOOL "" FORCE)
set(REPROC_INSTALL OFF CACHE BOOL "" FORCE)
FetchContent_Declare(reproc
  GIT_REPOSITORY https://github.com/DaanDeMeyer/reproc.git
  GIT_TAG v14.2.7
  GIT_SHALLOW TRUE)

if(XINSIGHT_BUILD_TESTS)
  set(DOCTEST_WITH_TESTS OFF CACHE BOOL "" FORCE)
  set(DOCTEST_NO_INSTALL ON CACHE BOOL "" FORCE)
  FetchContent_Declare(doctest
    GIT_REPOSITORY https://github.com/doctest/doctest.git
    GIT_TAG v2.5.3
    GIT_SHALLOW TRUE)
endif()

set(_xinsight_core_deps tree_sitter tree_sitter_c tree_sitter_cpp nlohmann_json reproc)
if(XINSIGHT_BUILD_TESTS)
  list(APPEND _xinsight_core_deps doctest)
endif()
FetchContent_MakeAvailable(${_xinsight_core_deps})
unset(_xinsight_core_deps)
unset(_xinsight_grammar_patch)
