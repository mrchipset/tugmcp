# Registered as `amalgamation.http` when the external requirement is ON but the
# configure-time toolchain cannot launch the Python runner. The test must fail
# loudly instead of disappearing from the ctest list.
if(NOT DEFINED REASON)
  set(REASON "external amalgamation verification is required but unavailable")
endif()
message(FATAL_ERROR
  "amalgamation.http: ${REASON}. "
  "Install the missing prerequisite, or configure with "
  "-DTUGMCP_REQUIRE_EXTERNAL_AMALGAMATION=OFF to skip external checks.")
