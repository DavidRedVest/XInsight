# Vendors QScintilla 2.14.1 (GPLv3 -- see /THIRD_PARTY_LICENSES) as a static
# library built directly against the project's Qt 6 install. QScintilla ships
# no CMake build (qmake .pro only), so this hand-writes one from its
# src/qscintilla.pro SOURCES/HEADERS/DEFINES/INCLUDEPATH lists (verified
# against the actual 2.14.1 tarball, not guessed) rather than depending on
# qmake being present.

include(FetchContent)

FetchContent_Declare(qscintilla_src
  URL https://www.riverbankcomputing.com/static/Downloads/QScintilla/2.14.1/QScintilla_src-2.14.1.tar.gz
  URL_HASH SHA256=dfe13c6acc9d85dfcba76ccc8061e71a223957a6c02f3c343b30a9d43a4cdd4d
  DOWNLOAD_EXTRACT_TIMESTAMP TRUE)

FetchContent_MakeAvailable(qscintilla_src)

set(XINSIGHT_QSCI_SRC_DIR ${qscintilla_src_SOURCE_DIR}/src)
set(XINSIGHT_SCI_DIR ${qscintilla_src_SOURCE_DIR}/scintilla)

file(GLOB XINSIGHT_QSCI_WRAPPER_SOURCES ${XINSIGHT_QSCI_SRC_DIR}/*.cpp)
# Q_OBJECT-bearing headers live in Qsci/, a different directory from their
# .cpp counterparts in src/ -- AUTOMOC's default same-directory/same-basename
# discovery heuristic misses them, so list them explicitly as sources to
# force moc processing (verified: without this, linking fails with
# undefined symbols for QsciScintilla's signals/qt_metacall/vtable).
file(GLOB XINSIGHT_QSCI_PUBLIC_HEADERS ${XINSIGHT_QSCI_SRC_DIR}/Qsci/*.h)
file(GLOB XINSIGHT_SCI_LEXER_SOURCES ${XINSIGHT_SCI_DIR}/lexers/*.cpp)
# LexLPeg.cpp needs a Lua toolchain and is deliberately absent from
# qscintilla.pro's own SOURCES list -- excluded here to match upstream.
list(REMOVE_ITEM XINSIGHT_SCI_LEXER_SOURCES ${XINSIGHT_SCI_DIR}/lexers/LexLPeg.cpp)
file(GLOB XINSIGHT_SCI_LEXLIB_SOURCES ${XINSIGHT_SCI_DIR}/lexlib/*.cpp)
file(GLOB XINSIGHT_SCI_CORE_SOURCES ${XINSIGHT_SCI_DIR}/src/*.cpp)

add_library(qscintilla2-xinsight STATIC
  ${XINSIGHT_QSCI_WRAPPER_SOURCES}
  ${XINSIGHT_QSCI_PUBLIC_HEADERS}
  ${XINSIGHT_SCI_LEXER_SOURCES}
  ${XINSIGHT_SCI_LEXLIB_SOURCES}
  ${XINSIGHT_SCI_CORE_SOURCES}
)

target_include_directories(qscintilla2-xinsight
  PUBLIC
    ${XINSIGHT_QSCI_SRC_DIR}
    # Consumers need this too: calling SendScintilla() directly (which is
    # how EditorView drives tree-sitter-based styling/folding) requires the
    # SCI_* message constants from Scintilla.h, which qsciscintillabase.h
    # deliberately does not include on consumers' behalf.
    ${XINSIGHT_SCI_DIR}/include
  PRIVATE
    ${XINSIGHT_SCI_DIR}/lexlib
    ${XINSIGHT_SCI_DIR}/src
)

target_compile_definitions(qscintilla2-xinsight PRIVATE
  SCINTILLA_QT
  SCI_LEXER
  INCLUDE_DEPRECATED_FEATURES
)

target_compile_features(qscintilla2-xinsight PUBLIC cxx_std_20)

target_link_libraries(qscintilla2-xinsight PUBLIC Qt6::Widgets Qt6::PrintSupport)

# This is upstream source we don't own; don't let its warnings drown out
# xinsight-qt's own.
if(NOT MSVC)
  target_compile_options(qscintilla2-xinsight PRIVATE -w)
endif()
