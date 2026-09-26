# The Android port compiles the portable C++ core straight from ../../../src,
# the same files the macOS app and the GTK port build. Nothing is copied here:
# there is one source of truth for highlighting, Markdown, settings and the
# rest, and the unit tests in tests/run_tests.cpp cover all of it.
cmake_minimum_required(VERSION 3.22)
project(minicode)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CORE ${CMAKE_CURRENT_SOURCE_DIR}/../../../../../src)
# The Source Control panel's model is the GTK port's GitModel, which is plain
# C++ with no GTK in it (tested in linux/tests/run_tests.cpp).
set(LINUX ${CMAKE_CURRENT_SOURCE_DIR}/../../../../../linux/src)

add_library(minicode SHARED
    minicode_jni.cpp
    terminal_jni.cpp
    latex_jni.cpp
    lsp_jni.cpp
    links_jni.cpp
    git_jni.cpp
    ${CORE}/SyntaxHighlighter.cpp
    ${CORE}/MarkdownParser.cpp
    ${CORE}/LineComments.cpp
    ${CORE}/Settings.cpp
    ${CORE}/TerminalStream.cpp
    ${CORE}/TerminalScreen.cpp
    ${CORE}/LatexDoc.cpp
    ${CORE}/SyncTex.cpp
    ${CORE}/Json.cpp
    ${CORE}/LspClient.cpp
    ${CORE}/TermLinks.cpp
    ${CORE}/GitStatus.cpp
    ${CORE}/GitGraph.cpp
    ${LINUX}/GitModel.cpp)

target_include_directories(minicode PRIVATE ${CORE})
target_compile_options(minicode PRIVATE -Wall -Wextra)
# zlib is part of the NDK's system libraries; SyncTeX files are gzipped.
target_link_libraries(minicode log z)
