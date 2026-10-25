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

# Real thread-pool tests run on the integration line with a wider timeout so a
# loaded PR machine cannot turn them into false failures (designs/testing.md §4.3).
add_executable(tugmcp_executor_tests
  main.cpp
  unit/executor_test.cpp)
target_link_libraries(tugmcp_executor_tests PRIVATE tugmcp::core tugmcp::tests)
doctest_discover_tests(tugmcp_executor_tests TEST_PREFIX executor.
  PROPERTIES LABELS integration TIMEOUT 10
  WORKING_DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}")

if(TUGMCP_TLS OR TUGMCP_ENABLE_LEGACY_SSE)
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
  target_sources(tugmcp_integration_tests PRIVATE
    integration/http_loopback_test.cpp
    integration/http_sse_test.cpp
    integration/client_2026_loopback_test.cpp
    integration/subscriptions_test.cpp
    integration/http_session_test.cpp
    integration/client_2025_loopback_test.cpp
    integration/security_test.cpp)
  if(TUGMCP_ENABLE_LEGACY_SSE)
    target_sources(tugmcp_integration_tests PRIVATE
      integration/legacy_sse_test.cpp)
  endif()
  target_link_libraries(tugmcp_integration_tests PRIVATE
    tugmcp::http tugmcp_3rd_httplib)
  if(TUGMCP_TLS)
    target_sources(tugmcp_integration_tests PRIVATE integration/tls_loopback_test.cpp)
  endif()
endif()

find_package(Python3 COMPONENTS Interpreter QUIET)
if(Python3_Interpreter_FOUND)
  add_test(NAME amalgamation.core
    COMMAND ${Python3_EXECUTABLE}
      ${CMAKE_CURRENT_SOURCE_DIR}/amalgamation/run_tests.py
      --out ${CMAKE_BINARY_DIR}/amalgamation/tugmcp.hpp
      --hash-file ${CMAKE_CURRENT_SOURCE_DIR}/amalgamation/core.sha256
      --json-include ${PROJECT_SOURCE_DIR}/3rd/nlohmann_json
      --build-dir ${CMAKE_CURRENT_BINARY_DIR}/amalgamation-build)
  set_tests_properties(amalgamation.core PROPERTIES
    LABELS amalgamation
    TIMEOUT 600
    RUN_SERIAL TRUE
    WORKING_DIRECTORY ${PROJECT_SOURCE_DIR})
endif()

# External Inspector + conformance verification of the HTTP/HTTP-TLS artifacts.
# With TUGMCP_REQUIRE_EXTERNAL_AMALGAMATION=ON (default) the test is registered
# even when prerequisites are absent; it then fails loudly at test time instead
# of silently disappearing from the ctest list.
if(TUGMCP_REQUIRE_EXTERNAL_AMALGAMATION)
  set(TUGMCP_AMALGAMATION_EXTERNAL_REASON "")
  if(NOT Python3_Interpreter_FOUND)
    set(TUGMCP_AMALGAMATION_EXTERNAL_REASON
      "Python 3 interpreter was not found at configure time")
  elseif(NOT TUGMCP_WITH_HTTP)
    set(TUGMCP_AMALGAMATION_EXTERNAL_REASON
      "TUGMCP_WITH_HTTP=OFF, but the HTTP single-header artifact is required")
  endif()

  if(TUGMCP_AMALGAMATION_EXTERNAL_REASON)
    add_test(NAME amalgamation.http
      COMMAND ${CMAKE_COMMAND}
        "-DREASON=${TUGMCP_AMALGAMATION_EXTERNAL_REASON}"
        -P ${CMAKE_CURRENT_SOURCE_DIR}/amalgamation/require_external.cmake)
  else()
    add_test(NAME amalgamation.http
      COMMAND ${Python3_EXECUTABLE}
        ${CMAKE_CURRENT_SOURCE_DIR}/amalgamation/run_external.py
        --core-out ${CMAKE_BINARY_DIR}/amalgamation/http/tugmcp.hpp
        --http-out ${CMAKE_BINARY_DIR}/amalgamation/http/tugmcp_http.hpp
        --http-tls-out ${CMAKE_BINARY_DIR}/amalgamation/http/tugmcp_http_tls.hpp
        --core-hash ${CMAKE_CURRENT_SOURCE_DIR}/amalgamation/core.sha256
        --http-hash ${CMAKE_CURRENT_SOURCE_DIR}/amalgamation/http.sha256
        --http-tls-hash ${CMAKE_CURRENT_SOURCE_DIR}/amalgamation/http_tls.sha256
        --json-include ${PROJECT_SOURCE_DIR}/3rd/nlohmann_json
        --httplib-include ${PROJECT_SOURCE_DIR}/3rd/cpp-httplib
        --build-dir ${CMAKE_CURRENT_BINARY_DIR}/amalgamation-http-build
        --conformance-out ${CMAKE_BINARY_DIR}/amalgamation/http/conformance)
  endif()
  set_tests_properties(amalgamation.http PROPERTIES
    LABELS amalgamation
    TIMEOUT 1800
    RUN_SERIAL TRUE
    WORKING_DIRECTORY ${PROJECT_SOURCE_DIR})
endif()

if(TUGMCP_CONFORMANCE)
  add_executable(tugmcp_conformance_server conformance/fixture_server.cpp)
  target_link_libraries(tugmcp_conformance_server PRIVATE
    tugmcp::core tugmcp::http)
  target_compile_features(tugmcp_conformance_server PRIVATE cxx_std_17)

  add_test(NAME conformance.external
    COMMAND ${Python3_EXECUTABLE}
      ${CMAKE_CURRENT_SOURCE_DIR}/conformance/run_external.py
      --fixture-server $<TARGET_FILE:tugmcp_conformance_server>
      --revision 2026-07-28
      --out ${CMAKE_BINARY_DIR}/conformance/2026-07-28
      --expected-failures ${CMAKE_CURRENT_SOURCE_DIR}/conformance/expected_failures.yaml)
  set_tests_properties(conformance.external PROPERTIES
    LABELS conformance
    TIMEOUT 1800
    WORKING_DIRECTORY ${CMAKE_SOURCE_DIR})

  add_test(NAME conformance.external.2025
    COMMAND ${Python3_EXECUTABLE}
      ${CMAKE_CURRENT_SOURCE_DIR}/conformance/run_external.py
      --fixture-server $<TARGET_FILE:tugmcp_conformance_server>
      --revision 2025-11-25
      --out ${CMAKE_BINARY_DIR}/conformance/2025-11-25
      --expected-failures ${CMAKE_CURRENT_SOURCE_DIR}/conformance/expected_failures.yaml)
  set_tests_properties(conformance.external.2025 PROPERTIES
    LABELS conformance
    TIMEOUT 1800
    WORKING_DIRECTORY ${CMAKE_SOURCE_DIR})
endif()
