cmake_minimum_required(VERSION 3.20)
project(tugmcp_amalgamation_consumer LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

if(NOT TUGMCP_AMALGAMATION_HEADER)
  message(FATAL_ERROR "TUGMCP_AMALGAMATION_HEADER=<path to generated header> is required")
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

if(MSVC)
  target_compile_options(tugmcp_amalgamation_consumer PRIVATE
    /W4 /permissive- /utf-8)
else()
  target_compile_options(tugmcp_amalgamation_consumer PRIVATE
    -Wall -Wextra -Wpedantic)
endif()

enable_testing()
add_test(NAME amalgamation.smoke COMMAND tugmcp_amalgamation_consumer)
set_tests_properties(amalgamation.smoke PROPERTIES TIMEOUT 60)
