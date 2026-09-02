# Standalone script (run via `cmake -P`) that embeds a .scm query file's
# contents into a generated header as a constexpr std::string_view. Run at
# build time (not configure time) via add_custom_command so editing a .scm
# file and rebuilding picks up the change without a manual reconfigure.
#
# Usage: cmake -DIN=<path> -DOUT=<path> -DVAR=<cpp identifier> -P EmbedQuery.cmake

if(NOT IN OR NOT OUT OR NOT VAR)
  message(FATAL_ERROR "EmbedQuery.cmake requires -DIN=<scm path> -DOUT=<generated header path> -DVAR=<cpp identifier>")
endif()

file(READ "${IN}" _xinsight_query_content)

file(WRITE "${OUT}" "// Auto-generated from ${IN} by xinsight-core/cmake/EmbedQuery.cmake. Do not edit.
#pragma once

#include <string_view>

namespace xinsight::core::queries {

constexpr std::string_view ${VAR} = R\"XINSIGHT_QUERY(${_xinsight_query_content})XINSIGHT_QUERY\";

} // namespace xinsight::core::queries
")
