# xinsight-core: pure C++20 business logic. This target must NEVER
# find_package(Qt6) or #include <Q...> anywhere -- that is the architectural
# litmus test from PRD 5.5/10. It must build and its tests must pass with
# zero GUI dependencies (see xinsight-core/tests).

include(${CMAKE_CURRENT_SOURCE_DIR}/cmake/Dependencies.cmake)

add_subdirectory(query)

add_library(xinsight-core STATIC
  src/Version.cpp
  src/intel/TreeSitterEngine.cpp
  src/intel/InMemorySymbolIndex.cpp
  src/intel/CodeIntelligence.cpp
  src/context/ContextEngine.cpp
  src/lsp/LspClient.cpp
  src/lsp/ClangdProvider.cpp
  src/project/ProjectModel.cpp
  src/encoding/TextCodec.cpp
  src/search/SearchEngine.cpp
  src/nav/NavigationEngine.cpp
  src/theme/Theme.cpp
)

add_dependencies(xinsight-core xinsight-core-generated-queries)

target_include_directories(xinsight-core
  PUBLIC
    $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
  PRIVATE
    ${XINSIGHT_QUERY_GEN_INCLUDE_DIR}
)

target_compile_features(xinsight-core PUBLIC cxx_std_20)

set(THREADS_PREFER_PTHREAD_FLAG ON)
find_package(Threads REQUIRED)
find_library(ICONV_LIBRARY iconv REQUIRED)

target_link_libraries(xinsight-core
  PUBLIC
    nlohmann_json::nlohmann_json
  PRIVATE
    tree-sitter
    tree-sitter-c
    tree-sitter-cpp
    reproc
    reproc++
    Threads::Threads
    ${ICONV_LIBRARY}
)

if(XINSIGHT_BUILD_TESTS)
  enable_testing()
  add_subdirectory(tests)
endif()
