cmake_minimum_required(VERSION 3.20)
project(tugmcp_amalgamation_consumer LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

enable_testing()

if(NOT TUGMCP_AMALGAMATION_HEADER)
  message(FATAL_ERROR "TUGMCP_AMALGAMATION_HEADER=<path to generated core header> is required")
endif()
if(NOT TUGMCP_JSON_INCLUDE)
  message(FATAL_ERROR "TUGMCP_JSON_INCLUDE=<path containing nlohmann/json.hpp> is required")
endif()

get_filename_component(TUGMCP_AMALGAMATION_HEADER
  "${TUGMCP_AMALGAMATION_HEADER}" ABSOLUTE)
get_filename_component(TUGMCP_JSON_INCLUDE "${TUGMCP_JSON_INCLUDE}" ABSOLUTE)
get_filename_component(TUGMCP_AMALGAMATION_DIR
  "${TUGMCP_AMALGAMATION_HEADER}" DIRECTORY)
get_filename_component(TUGMCP_AMALGAMATION_NAME
  "${TUGMCP_AMALGAMATION_HEADER}" NAME)

if(NOT EXISTS "${TUGMCP_AMALGAMATION_HEADER}")
  message(FATAL_ERROR "Generated header not found: ${TUGMCP_AMALGAMATION_HEADER}")
endif()
if(NOT EXISTS "${TUGMCP_JSON_INCLUDE}/nlohmann/json.hpp")
  message(FATAL_ERROR
    "TUGMCP_JSON_INCLUDE does not contain nlohmann/json.hpp: ${TUGMCP_JSON_INCLUDE}")
endif()

find_package(Threads REQUIRED)

function(tugmcp_amalgamation_warnings target)
  if(MSVC)
    # The HTTP artifact merges cpp-httplib and the full library into one TU.
    target_compile_options(${target} PRIVATE /W4 /permissive- /utf-8 /bigobj)
  else()
    target_compile_options(${target} PRIVATE -Wall -Wextra -Wpedantic)
  endif()
endfunction()

# Point at the generated header directly (no configure-time copy, so a rebuilt
# header can never go stale relative to the configured project).
add_executable(tugmcp_amalgamation_consumer
  consumer_core.cpp
  consumer_core_tu2.cpp)
target_include_directories(tugmcp_amalgamation_consumer PRIVATE
  "${TUGMCP_AMALGAMATION_DIR}"
  "${TUGMCP_JSON_INCLUDE}")
target_compile_definitions(tugmcp_amalgamation_consumer PRIVATE
  TUGMCP_AMALGAMATION_HEADER_NAME="${TUGMCP_AMALGAMATION_NAME}")
target_link_libraries(tugmcp_amalgamation_consumer PRIVATE Threads::Threads)
tugmcp_amalgamation_warnings(tugmcp_amalgamation_consumer)

# Stdio echo server built from the generated core header (MCP Inspector).
add_executable(tugmcp_amalgamation_stdio_server consumer_stdio_server.cpp)
target_include_directories(tugmcp_amalgamation_stdio_server PRIVATE
  "${TUGMCP_AMALGAMATION_DIR}"
  "${TUGMCP_JSON_INCLUDE}")
target_link_libraries(tugmcp_amalgamation_stdio_server PRIVATE Threads::Threads)
tugmcp_amalgamation_warnings(tugmcp_amalgamation_stdio_server)

if(TUGMCP_AMALGAMATION_HTTP_HEADER)
  if(NOT TUGMCP_HTTPLIB_INCLUDE)
    message(FATAL_ERROR
      "TUGMCP_HTTPLIB_INCLUDE=<path containing httplib.h> is required with "
      "TUGMCP_AMALGAMATION_HTTP_HEADER")
  endif()
  get_filename_component(TUGMCP_AMALGAMATION_HTTP_HEADER
    "${TUGMCP_AMALGAMATION_HTTP_HEADER}" ABSOLUTE)
  get_filename_component(TUGMCP_HTTPLIB_INCLUDE
    "${TUGMCP_HTTPLIB_INCLUDE}" ABSOLUTE)
  get_filename_component(TUGMCP_AMALGAMATION_HTTP_DIR
    "${TUGMCP_AMALGAMATION_HTTP_HEADER}" DIRECTORY)
  if(NOT EXISTS "${TUGMCP_AMALGAMATION_HTTP_HEADER}")
    message(FATAL_ERROR
      "Generated HTTP header not found: ${TUGMCP_AMALGAMATION_HTTP_HEADER}")
  endif()
  if(NOT EXISTS "${TUGMCP_HTTPLIB_INCLUDE}/httplib.h")
    message(FATAL_ERROR
      "TUGMCP_HTTPLIB_INCLUDE does not contain httplib.h: ${TUGMCP_HTTPLIB_INCLUDE}")
  endif()

  # Declaration-only TU: no cpp-httplib include path is provided on purpose.
  add_library(tugmcp_amalgamation_http_tu2 OBJECT consumer_http_tu2.cpp)
  target_include_directories(tugmcp_amalgamation_http_tu2 PRIVATE
    "${TUGMCP_AMALGAMATION_HTTP_DIR}"
    "${TUGMCP_JSON_INCLUDE}")
  target_compile_definitions(tugmcp_amalgamation_http_tu2 PRIVATE
    TUGMCP_HTTP_DECL_HEADER="tugmcp_http.hpp")
  tugmcp_amalgamation_warnings(tugmcp_amalgamation_http_tu2)

  # Streamable HTTP echo server built from the generated HTTP header. The
  # declaration-only TU is linked in to prove httplib-free ODR safety.
  add_executable(tugmcp_amalgamation_http_server
    consumer_http_server.cpp
    $<TARGET_OBJECTS:tugmcp_amalgamation_http_tu2>)
  target_include_directories(tugmcp_amalgamation_http_server PRIVATE
    "${TUGMCP_AMALGAMATION_HTTP_DIR}"
    "${TUGMCP_JSON_INCLUDE}"
    "${TUGMCP_HTTPLIB_INCLUDE}")
  target_link_libraries(tugmcp_amalgamation_http_server PRIVATE
    Threads::Threads)
  if(WIN32)
    target_link_libraries(tugmcp_amalgamation_http_server PRIVATE ws2_32)
  endif()
  tugmcp_amalgamation_warnings(tugmcp_amalgamation_http_server)

  # Conformance fixture server built from the same generated HTTP header.
  add_executable(tugmcp_amalgamation_fixture_server
    "${CMAKE_CURRENT_SOURCE_DIR}/../conformance/fixture_server.cpp")
  target_compile_definitions(tugmcp_amalgamation_fixture_server PRIVATE
    TUGMCP_USE_AMALGAMATION)
  target_include_directories(tugmcp_amalgamation_fixture_server PRIVATE
    "${TUGMCP_AMALGAMATION_HTTP_DIR}"
    "${TUGMCP_JSON_INCLUDE}"
    "${TUGMCP_HTTPLIB_INCLUDE}")
  target_link_libraries(tugmcp_amalgamation_fixture_server PRIVATE
    Threads::Threads)
  if(WIN32)
    target_link_libraries(tugmcp_amalgamation_fixture_server PRIVATE ws2_32)
  endif()
  tugmcp_amalgamation_warnings(tugmcp_amalgamation_fixture_server)
endif()

if(TUGMCP_AMALGAMATION_HTTP_TLS_HEADER)
  if(NOT TUGMCP_HTTPLIB_INCLUDE)
    message(FATAL_ERROR
      "TUGMCP_HTTPLIB_INCLUDE=<path containing httplib.h> is required with "
      "TUGMCP_AMALGAMATION_HTTP_TLS_HEADER")
  endif()
  get_filename_component(TUGMCP_AMALGAMATION_HTTP_TLS_HEADER
    "${TUGMCP_AMALGAMATION_HTTP_TLS_HEADER}" ABSOLUTE)
  get_filename_component(TUGMCP_AMALGAMATION_HTTP_TLS_DIR
    "${TUGMCP_AMALGAMATION_HTTP_TLS_HEADER}" DIRECTORY)
  get_filename_component(TUGMCP_AMALGAMATION_HTTP_TLS_NAME
    "${TUGMCP_AMALGAMATION_HTTP_TLS_HEADER}" NAME)
  if(NOT EXISTS "${TUGMCP_AMALGAMATION_HTTP_TLS_HEADER}")
    message(FATAL_ERROR
      "Generated HTTP TLS header not found: ${TUGMCP_AMALGAMATION_HTTP_TLS_HEADER}")
  endif()
  if(NOT EXISTS "${TUGMCP_HTTPLIB_INCLUDE}/httplib.h")
    message(FATAL_ERROR
      "TUGMCP_HTTPLIB_INCLUDE does not contain httplib.h: ${TUGMCP_HTTPLIB_INCLUDE}")
  endif()

  find_package(OpenSSL 3.0 REQUIRED COMPONENTS SSL Crypto)

  add_library(tugmcp_amalgamation_http_tls_tu2 OBJECT consumer_http_tu2.cpp)
  target_include_directories(tugmcp_amalgamation_http_tls_tu2 PRIVATE
    "${TUGMCP_AMALGAMATION_HTTP_TLS_DIR}"
    "${TUGMCP_JSON_INCLUDE}")
  target_compile_definitions(tugmcp_amalgamation_http_tls_tu2 PRIVATE
    TUGMCP_HTTP_DECL_HEADER="${TUGMCP_AMALGAMATION_HTTP_TLS_NAME}")
  tugmcp_amalgamation_warnings(tugmcp_amalgamation_http_tls_tu2)

  add_executable(tugmcp_amalgamation_http_tls_odr
    consumer_http_tls.cpp
    $<TARGET_OBJECTS:tugmcp_amalgamation_http_tls_tu2>)
  target_compile_definitions(tugmcp_amalgamation_http_tls_odr PRIVATE
    TUGMCP_AMALGAMATION_HTTP_TLS_HEADER_NAME="${TUGMCP_AMALGAMATION_HTTP_TLS_NAME}")
  target_include_directories(tugmcp_amalgamation_http_tls_odr PRIVATE
    "${TUGMCP_AMALGAMATION_HTTP_TLS_DIR}"
    "${TUGMCP_JSON_INCLUDE}"
    "${TUGMCP_HTTPLIB_INCLUDE}")
  target_link_libraries(tugmcp_amalgamation_http_tls_odr PRIVATE
    Threads::Threads OpenSSL::SSL OpenSSL::Crypto)
  if(WIN32)
    target_link_libraries(tugmcp_amalgamation_http_tls_odr PRIVATE
      ws2_32 crypt32)
  endif()
  tugmcp_amalgamation_warnings(tugmcp_amalgamation_http_tls_odr)

  add_test(NAME amalgamation.http.tls.odr
    COMMAND tugmcp_amalgamation_http_tls_odr)
  set_tests_properties(amalgamation.http.tls.odr PROPERTIES TIMEOUT 60)
endif()

add_test(NAME amalgamation.smoke COMMAND tugmcp_amalgamation_consumer)
set_tests_properties(amalgamation.smoke PROPERTIES TIMEOUT 60)
