add_library(tugmcp_3rd_json INTERFACE)
target_include_directories(tugmcp_3rd_json SYSTEM INTERFACE
  ${CMAKE_CURRENT_SOURCE_DIR}/nlohmann_json)
target_compile_features(tugmcp_3rd_json INTERFACE cxx_std_17)

add_library(tugmcp_3rd_doctest INTERFACE)
target_include_directories(tugmcp_3rd_doctest SYSTEM INTERFACE
  ${CMAKE_CURRENT_SOURCE_DIR}/doctest)
target_compile_features(tugmcp_3rd_doctest INTERFACE cxx_std_17)

add_library(tugmcp_3rd_jsoncons INTERFACE)
target_include_directories(tugmcp_3rd_jsoncons SYSTEM INTERFACE
  ${CMAKE_CURRENT_SOURCE_DIR}/jsoncons/include)
target_compile_features(tugmcp_3rd_jsoncons INTERFACE cxx_std_17)

add_library(tugmcp_3rd_valijson INTERFACE)
target_include_directories(tugmcp_3rd_valijson SYSTEM INTERFACE
  ${CMAKE_CURRENT_SOURCE_DIR}/valijson/include)
target_compile_features(tugmcp_3rd_valijson INTERFACE cxx_std_17)

add_library(tugmcp_3rd_httplib INTERFACE)
target_include_directories(tugmcp_3rd_httplib SYSTEM INTERFACE
  ${CMAKE_CURRENT_SOURCE_DIR}/cpp-httplib)
target_compile_features(tugmcp_3rd_httplib INTERFACE cxx_std_17)
