add_executable(xinsight-core-tests
  version_test.cpp
  ImmediateUiDispatcher_test.cpp
  TreeSitterEngine_test.cpp
  ProjectModel_test.cpp
  TextCodec_test.cpp
  SearchEngine_test.cpp
  NavigationEngine_test.cpp
  Theme_test.cpp
  SymbolIndex_test.cpp
  CodeIntelligence_test.cpp
  ContextEngine_test.cpp
  LspClient_test.cpp
  ClangdProvider_test.cpp
)

target_link_libraries(xinsight-core-tests PRIVATE
  xinsight-core
  doctest::doctest
)

target_include_directories(xinsight-core-tests PRIVATE
  ${CMAKE_CURRENT_SOURCE_DIR}/support
)

include(CTest)
add_test(NAME xinsight-core-tests COMMAND xinsight-core-tests)
