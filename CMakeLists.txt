cmake_minimum_required(VERSION 3.21)

project(XInsight
        VERSION 0.4.0
        DESCRIPTION "Source Insight-style C/C++ code reader + lightweight editor for macOS"
        LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

# Vendored third-party sources are compiled straight into xinsight-core and
# bundled into the app; static libs keep the whole thing self-contained.
set(BUILD_SHARED_LIBS OFF CACHE BOOL "" FORCE)

if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
  set(CMAKE_BUILD_TYPE RelWithDebInfo CACHE STRING "Build type" FORCE)
endif()

option(XINSIGHT_BUILD_TESTS "Build xinsight-core headless unit tests" ON)
option(XINSIGHT_BUILD_QT "Build the xinsight-qt GUI frontend" ON)

# xinsight-core must remain buildable and testable with zero GUI dependencies.
# This target is always available regardless of XINSIGHT_BUILD_QT.
add_subdirectory(xinsight-core)

if(XINSIGHT_BUILD_QT)
  add_subdirectory(xinsight-qt)
endif()
