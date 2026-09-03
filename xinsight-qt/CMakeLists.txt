# xinsight-qt: Qt 6 GUI frontend. Rendering + input only -- must never
# assemble LSP JSON, spawn rg/clangd directly, or call tree-sitter/clangd
# directly. All of that goes through xinsight-core's single exit points
# (CodeIntelligence / SearchEngine / NavigationEngine / LspClient).

find_package(Qt6 REQUIRED COMPONENTS Widgets PrintSupport Svg)

qt_standard_project_setup()
set(CMAKE_AUTORCC ON)

add_subdirectory(third_party/qscintilla)

# MACOSX_BUNDLE: without this, the built binary is a bare Unix executable,
# and macOS launches those by opening Terminal.app and running them there --
# double-clicking (or Spotlight/Dock-launching) xinsight-qt would pop a
# console window on every run. Wrapping it as a real .app bundle (even
# unsigned/un-notarized -- that step is still deferred, see PRD 9) makes
# Finder/LaunchServices treat it as a GUI app and launch it directly.
add_executable(xinsight-qt MACOSX_BUNDLE
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
  src/ContextPaneView.cpp
  src/ClangdStatusView.cpp
  resources/resources.qrc
)

set_target_properties(xinsight-qt PROPERTIES
  MACOSX_BUNDLE_BUNDLE_NAME "XInsight"
  MACOSX_BUNDLE_GUI_IDENTIFIER "dev.davidredvest.xinsight"
  MACOSX_BUNDLE_BUNDLE_VERSION "${PROJECT_VERSION}"
  MACOSX_BUNDLE_SHORT_VERSION_STRING "${PROJECT_VERSION}"
)

target_include_directories(xinsight-qt PRIVATE src)

target_link_libraries(xinsight-qt PRIVATE
  xinsight-core
  Qt6::Widgets
  Qt6::Svg
  qscintilla2-xinsight
)
