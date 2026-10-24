add_executable(tugmcp_echo_server echo_server/main.cpp)
target_link_libraries(tugmcp_echo_server PRIVATE tugmcp::core)
target_compile_features(tugmcp_echo_server PRIVATE cxx_std_17)

if(TUGMCP_WITH_HTTP)
  target_link_libraries(tugmcp_echo_server PRIVATE tugmcp::http)
  target_compile_definitions(tugmcp_echo_server PRIVATE TUGMCP_ECHO_HTTP=1)

  add_executable(tugmcp_echo_client echo_client/main.cpp)
  target_link_libraries(tugmcp_echo_client PRIVATE tugmcp::http tugmcp::core)
  target_compile_features(tugmcp_echo_client PRIVATE cxx_std_17)
endif()
