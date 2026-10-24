list(APPEND CMAKE_MODULE_PATH "${PROJECT_SOURCE_DIR}/cmake")
include(doctest)

add_library(tugmcp_tests INTERFACE)
target_include_directories(tugmcp_tests INTERFACE
  ${CMAKE_CURRENT_SOURCE_DIR}/helpers)
target_link_libraries(tugmcp_tests INTERFACE tugmcp_3rd_doctest)
add_library(tugmcp::tests ALIAS tugmcp_tests)

add_executable(tugmcp_unit_tests
  main.cpp
  unit/request_id_test.cpp
  unit/error_test.cpp
  unit/codec_test.cpp
  unit/version_test.cpp
  unit/framing_test.cpp
  unit/registry_test.cpp
  unit/dispatcher_test.cpp
  unit/client_test.cpp
  unit/canonical_test.cpp
  unit/executor_test.cpp
  unit/schema_test.cpp
  unit/protocol_surface_test.cpp)
target_link_libraries(tugmcp_unit_tests PRIVATE tugmcp::core tugmcp::tests)
target_compile_definitions(tugmcp_unit_tests PRIVATE
  TUGMCP_TEST_DATA_DIR="${CMAKE_CURRENT_SOURCE_DIR}")
doctest_discover_tests(tugmcp_unit_tests TEST_PREFIX unit.
  PROPERTIES LABELS unit TIMEOUT 2 WORKING_DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}")

if(TUGMCP_WITH_JSONCONS)
  target_sources(tugmcp_unit_tests PRIVATE unit/jsoncons_schema_test.cpp)
  target_link_libraries(tugmcp_unit_tests PRIVATE tugmcp::schema_jsoncons)
endif()

if(TUGMCP_WITH_VALIJSON)
  target_sources(tugmcp_unit_tests PRIVATE unit/valijson_schema_test.cpp)
  target_link_libraries(tugmcp_unit_tests PRIVATE tugmcp::schema_valijson)
endif()

add_executable(tugmcp_odr_tests odr/main.cpp odr/tu_a.cpp odr/tu_b.cpp)
target_link_libraries(tugmcp_odr_tests PRIVATE tugmcp::core tugmcp::tests)
doctest_discover_tests(tugmcp_odr_tests TEST_PREFIX odr.
  PROPERTIES LABELS unit TIMEOUT 2 WORKING_DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}")

if(TUGMCP_TLS)
  set(TUGMCP_INTEGRATION_TIMEOUT 10)
else()
  set(TUGMCP_INTEGRATION_TIMEOUT 2)
endif()

add_executable(tugmcp_integration_tests
  main.cpp
  integration/server_test.cpp
  integration/stdio_loopback_test.cpp
  integration/client_loopback_test.cpp)
target_link_libraries(tugmcp_integration_tests PRIVATE tugmcp::core tugmcp::tests)
target_compile_definitions(tugmcp_integration_tests PRIVATE
  TUGMCP_TEST_DATA_DIR="${CMAKE_CURRENT_SOURCE_DIR}")
if(TUGMCP_TLS)
  target_compile_definitions(tugmcp_integration_tests PRIVATE
    TUGMCP_TLS_ENABLED=1
    CPPHTTPLIB_OPENSSL_SUPPORT)
  target_link_libraries(tugmcp_integration_tests PRIVATE
    OpenSSL::SSL OpenSSL::Crypto)
else()
  target_compile_definitions(tugmcp_integration_tests PRIVATE TUGMCP_TLS_ENABLED=0)
endif()
doctest_discover_tests(tugmcp_integration_tests TEST_PREFIX integration.
  PROPERTIES LABELS integration TIMEOUT ${TUGMCP_INTEGRATION_TIMEOUT}
  WORKING_DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}")

if(TUGMCP_WITH_HTTP)
  target_sources(tugmcp_integration_tests PRIVATE integration/http_loopback_test.cpp)
  target_link_libraries(tugmcp_integration_tests PRIVATE
    tugmcp::http tugmcp_3rd_httplib)
  if(TUGMCP_TLS)
    target_sources(tugmcp_integration_tests PRIVATE integration/tls_loopback_test.cpp)
  endif()
endif()
