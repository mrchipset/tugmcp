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

if(TUGMCP_BUILD_QT_EXAMPLE)
  find_package(Qt5 REQUIRED COMPONENTS Widgets)
  add_executable(tugmcp_qt_widgets_server qt_widgets_server/main.cpp)
  target_link_libraries(tugmcp_qt_widgets_server PRIVATE
    tugmcp::core tugmcp::http Qt5::Widgets)
  target_compile_features(tugmcp_qt_widgets_server PRIVATE cxx_std_17)
  if(MSVC)
    target_compile_options(tugmcp_qt_widgets_server PRIVATE /utf-8)
  endif()
endif()
