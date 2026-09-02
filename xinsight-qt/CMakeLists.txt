# xinsight-qt: Qt 6 GUI frontend. Rendering + input only -- must never
# assemble LSP JSON, spawn rg/clangd directly, or call tree-sitter/clangd
# directly. All of that goes through xinsight-core's single exit points
# (CodeIntelligence / SearchEngine / NavigationEngine / LspClient).

find_package(Qt6 REQUIRED COMPONENTS Widgets PrintSupport)

qt_standard_project_setup()

add_subdirectory(third_party/qscintilla)

add_executable(xinsight-qt
  src/main.cpp
  src/MainWindow.cpp
  src/QtUiDispatcher.cpp
  src/ProjectTreeView.cpp
  src/OutlineView.cpp
  src/EditorView.cpp
  src/EditorPane.cpp
  src/SplitManager.cpp
  src/HighlightStyles.cpp
  src/DocumentRegistry.cpp
  src/SearchPanel.cpp
)

target_include_directories(xinsight-qt PRIVATE src)

target_link_libraries(xinsight-qt PRIVATE
  xinsight-core
  Qt6::Widgets
  qscintilla2-xinsight
)
