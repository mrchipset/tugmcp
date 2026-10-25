cmake_minimum_required(VERSION 3.20)
project(tugmcp VERSION 0.1.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

option(TUGMCP_BUILD_TESTS "Build tugmcp test targets" ON)
option(TUGMCP_BUILD_EXAMPLES "Build tugmcp examples" ON)
option(TUGMCP_WITH_HTTP "Build the Streamable HTTP transport" ON)
option(TUGMCP_TLS "Enable TLS support (OpenSSL >= 3.0)" OFF)
option(TUGMCP_HEADER_ONLY "Consume the library header-only (not implemented yet)" OFF)
option(TUGMCP_REQUIRE_EXTERNAL_AMALGAMATION
  "Require single-header external Inspector/conformance tests (fails loudly if prerequisites are missing)" ON)
option(TUGMCP_USE_SYSTEM_DEPS "Use system packages instead of 3rd/ (not implemented yet)" OFF)
option(TUGMCP_VERIFY_3RD_HASHES "Verify SHA256 of vendored dependencies" ON)
option(TUGMCP_WITH_JSONCONS "Enable 2020-12 schema validation via jsoncons" OFF)
option(TUGMCP_WITH_VALIJSON "Enable draft-07 schema validation via Valijson" OFF)
option(TUGMCP_ENABLE_LEGACY_SSE "Build legacy HTTP+SSE adapter (not implemented yet)" OFF)
option(TUGMCP_CONFORMANCE "Run external MCP conformance suite (not implemented yet)" OFF)
set(TUGMCP_TLS_BACKEND "openssl" CACHE STRING "TLS backend (openssl only)")

if(TUGMCP_HEADER_ONLY)
  message(FATAL_ERROR "TUGMCP_HEADER_ONLY is not implemented yet")
endif()
if(TUGMCP_USE_SYSTEM_DEPS)
  message(FATAL_ERROR "TUGMCP_USE_SYSTEM_DEPS is not implemented yet")
endif()
if(TUGMCP_ENABLE_LEGACY_SSE AND NOT TUGMCP_WITH_HTTP)
  message(FATAL_ERROR "TUGMCP_ENABLE_LEGACY_SSE=ON requires TUGMCP_WITH_HTTP=ON")
endif()
if(TUGMCP_CONFORMANCE)
  if(NOT TUGMCP_WITH_HTTP)
    message(FATAL_ERROR "TUGMCP_CONFORMANCE requires TUGMCP_WITH_HTTP=ON")
  endif()
  if(NOT TUGMCP_BUILD_TESTS)
    message(FATAL_ERROR "TUGMCP_CONFORMANCE requires TUGMCP_BUILD_TESTS=ON")
  endif()
  find_package(Python3 COMPONENTS Interpreter REQUIRED)
  find_program(TUGMCP_NPX_EXECUTABLE NAMES npx npx.cmd REQUIRED)
endif()
if(NOT TUGMCP_TLS_BACKEND STREQUAL "openssl")
  message(FATAL_ERROR "TUGMCP_TLS_BACKEND '${TUGMCP_TLS_BACKEND}' is not implemented yet (M2); only 'openssl' is supported")
endif()

if(TUGMCP_TLS AND NOT TUGMCP_WITH_HTTP)
  message(FATAL_ERROR "TUGMCP_TLS=ON requires TUGMCP_WITH_HTTP=ON")
endif()

find_package(Threads REQUIRED)

if(TUGMCP_VERIFY_3RD_HASHES)
  include(${CMAKE_CURRENT_SOURCE_DIR}/cmake/verify_hashes.cmake)
endif()

add_subdirectory(3rd)

add_library(tugmcp_core STATIC
  src/core/version.cpp
  src/core/error.cpp
  src/core/message.cpp
  src/core/codec.cpp
  src/core/canonical.cpp
  src/core/operation.cpp
  src/core/registry.cpp
  src/core/dispatcher.cpp
  src/core/server.cpp
  src/core/executor.cpp
  src/core/client.cpp
  src/transport/stdio.cpp)
add_library(tugmcp::core ALIAS tugmcp_core)

target_include_directories(tugmcp_core PUBLIC
  $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>)
target_compile_features(tugmcp_core PUBLIC cxx_std_17)
target_link_libraries(tugmcp_core PUBLIC Threads::Threads)
# Public headers expose Json, and the library sources also need the include dir
# (an INTERFACE-only link would not apply to tugmcp_core itself).
target_link_libraries(tugmcp_core PUBLIC tugmcp_3rd_json)

if(MSVC)
  target_compile_options(tugmcp_core PRIVATE /W4 /permissive- /utf-8)
else()
  target_compile_options(tugmcp_core PRIVATE -Wall -Wextra -Wpedantic)
endif()

if(TUGMCP_WITH_JSONCONS)
  add_library(tugmcp_schema_jsoncons STATIC src/schema/jsoncons_validator.cpp)
  add_library(tugmcp::schema_jsoncons ALIAS tugmcp_schema_jsoncons)
  target_compile_features(tugmcp_schema_jsoncons PUBLIC cxx_std_17)
  target_link_libraries(tugmcp_schema_jsoncons PUBLIC tugmcp::core)
  target_link_libraries(tugmcp_schema_jsoncons PRIVATE tugmcp_3rd_jsoncons)
  if(MSVC)
    target_compile_options(tugmcp_schema_jsoncons PRIVATE /W4 /permissive- /utf-8)
  else()
    target_compile_options(tugmcp_schema_jsoncons PRIVATE -Wall -Wextra -Wpedantic)
  endif()
endif()

if(TUGMCP_WITH_VALIJSON)
  add_library(tugmcp_schema_valijson STATIC src/schema/valijson_validator.cpp)
  add_library(tugmcp::schema_valijson ALIAS tugmcp_schema_valijson)
  target_compile_features(tugmcp_schema_valijson PUBLIC cxx_std_17)
  target_link_libraries(tugmcp_schema_valijson PUBLIC tugmcp::core)
  target_link_libraries(tugmcp_schema_valijson PRIVATE tugmcp_3rd_valijson)
  if(MSVC)
    target_compile_options(tugmcp_schema_valijson PRIVATE /W4 /permissive- /utf-8)
  else()
    target_compile_options(tugmcp_schema_valijson PRIVATE -Wall -Wextra -Wpedantic)
  endif()
endif()

if(TUGMCP_WITH_HTTP)
  add_library(tugmcp_http STATIC
    src/transport/http_server.cpp
    src/transport/http_client.cpp
    $<$<BOOL:${TUGMCP_ENABLE_LEGACY_SSE}>:src/transport/legacy_sse.cpp>)
  add_library(tugmcp::http ALIAS tugmcp_http)
  target_compile_features(tugmcp_http PUBLIC cxx_std_17)
  target_link_libraries(tugmcp_http PUBLIC tugmcp::core)
  target_link_libraries(tugmcp_http PRIVATE tugmcp_3rd_httplib)
  if(TUGMCP_TLS)
    find_package(OpenSSL 3.0 REQUIRED COMPONENTS SSL Crypto)
    target_compile_definitions(tugmcp_http PRIVATE
      TUGMCP_TLS_ENABLED=1
      CPPHTTPLIB_OPENSSL_SUPPORT)
    target_link_libraries(tugmcp_http PRIVATE OpenSSL::SSL OpenSSL::Crypto)
    if(WIN32)
      target_link_libraries(tugmcp_http PRIVATE crypt32)
    endif()
  else()
    target_compile_definitions(tugmcp_http PRIVATE TUGMCP_TLS_ENABLED=0)
  endif()
  if(WIN32)
    target_link_libraries(tugmcp_http PRIVATE ws2_32)
  endif()
  if(MSVC)
    target_compile_options(tugmcp_http PRIVATE /W4 /permissive- /utf-8)
  else()
    target_compile_options(tugmcp_http PRIVATE -Wall -Wextra -Wpedantic)
  endif()
endif()

if(TUGMCP_BUILD_TESTS)
  enable_testing()
  add_subdirectory(tests)
endif()

if(TUGMCP_BUILD_EXAMPLES AND EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/examples/CMakeLists.txt")
  add_subdirectory(examples)
endif()
