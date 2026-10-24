include_guard(GLOBAL)

set(_tugmcp_vendored_doctest
  "${CMAKE_CURRENT_LIST_DIR}/../3rd/doctest/scripts/cmake/doctest.cmake")

if(EXISTS "${_tugmcp_vendored_doctest}")
  include("${_tugmcp_vendored_doctest}")
else()
  include(doctest)
endif()
