list(APPEND CMAKE_MODULE_PATH "${PROJECT_SOURCE_DIR}/cmake")
include(doctest)

add_library(tugmcp_tests INTERFACE)
target_link_libraries(tugmcp_tests INTERFACE tugmcp_3rd_doctest)
add_library(tugmcp::tests ALIAS tugmcp_tests)

add_executable(tugmcp_unit_tests
  main.cpp
  unit/request_id_test.cpp
  unit/error_test.cpp
  unit/codec_test.cpp
  unit/version_test.cpp)
target_link_libraries(tugmcp_unit_tests PRIVATE tugmcp::core tugmcp::tests)
target_compile_definitions(tugmcp_unit_tests PRIVATE
  TUGMCP_TEST_DATA_DIR="${CMAKE_CURRENT_SOURCE_DIR}")
doctest_discover_tests(tugmcp_unit_tests TEST_PREFIX unit.
  PROPERTIES LABELS unit TIMEOUT 2 WORKING_DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}")

add_executable(tugmcp_odr_tests odr/main.cpp odr/tu_a.cpp odr/tu_b.cpp)
target_link_libraries(tugmcp_odr_tests PRIVATE tugmcp::core tugmcp::tests)
doctest_discover_tests(tugmcp_odr_tests TEST_PREFIX odr.
  PROPERTIES LABELS unit TIMEOUT 2 WORKING_DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}")
