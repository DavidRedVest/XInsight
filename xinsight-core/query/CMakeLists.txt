# Embeds each .scm query file into a generated header so query text ships
# compiled into the xinsight-core binary (PRD 8.1) rather than being read
# from disk at runtime. Regenerates automatically on rebuild when a .scm
# changes (see cmake/EmbedQuery.cmake).

set(XINSIGHT_QUERY_GEN_DIR ${CMAKE_CURRENT_BINARY_DIR}/generated)
set(XINSIGHT_QUERY_GEN_INCLUDE_DIR ${XINSIGHT_QUERY_GEN_DIR} PARENT_SCOPE)

set(_xinsight_embed_script ${CMAKE_CURRENT_SOURCE_DIR}/../cmake/EmbedQuery.cmake)
set(_xinsight_query_headers "")

function(xinsight_embed_query LANG KIND VAR_NAME)
  set(_in ${CMAKE_CURRENT_SOURCE_DIR}/${LANG}/${KIND}.scm)
  set(_out ${XINSIGHT_QUERY_GEN_DIR}/xinsight/core/queries/${LANG}_${KIND}.h)
  add_custom_command(
    OUTPUT ${_out}
    COMMAND ${CMAKE_COMMAND} -DIN=${_in} -DOUT=${_out} -DVAR=${VAR_NAME} -P ${_xinsight_embed_script}
    DEPENDS ${_in} ${_xinsight_embed_script}
    COMMENT "xinsight-core: embedding query ${LANG}/${KIND}.scm"
    VERBATIM)
  set(_xinsight_query_headers ${_xinsight_query_headers} ${_out} PARENT_SCOPE)
endfunction()

xinsight_embed_query(c highlights kCHighlights)
xinsight_embed_query(c tags kCTags)
xinsight_embed_query(c folds kCFolds)
xinsight_embed_query(cpp highlights kCppHighlights)
xinsight_embed_query(cpp tags kCppTags)
xinsight_embed_query(cpp folds kCppFolds)

add_custom_target(xinsight-core-generated-queries DEPENDS ${_xinsight_query_headers})
