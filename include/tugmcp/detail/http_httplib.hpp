#pragma once

// Single inclusion point for cpp-httplib in header-only/amalgamation builds.
// The definition must be supplied by the consuming target; the checks below
// catch a CMake configuration that drifts from the compiled library.

#ifndef TUGMCP_TLS_ENABLED
#error "TUGMCP_TLS_ENABLED must be defined by the tugmcp::http target"
#endif

#include <httplib.h>

#if TUGMCP_TLS_ENABLED && !defined(CPPHTTPLIB_SSL_ENABLED)
#error "TLS enabled but cpp-httplib built without SSL support"
#endif

#if !TUGMCP_TLS_ENABLED && defined(CPPHTTPLIB_SSL_ENABLED)
#error "cpp-httplib SSL enabled but TUGMCP_TLS is off"
#endif
