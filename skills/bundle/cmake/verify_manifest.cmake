# Verify a per-file SHA256 manifest produced for multi-file vendored libraries.
# Manifest format: "<sha256>  <relative/path>" (two spaces), paths relative to
# the manifest's own directory.

function(tugmcp_verify_manifest manifest_file)
  if(NOT EXISTS "${manifest_file}")
    message(FATAL_ERROR "tugmcp: manifest is missing: ${manifest_file}")
  endif()

  get_filename_component(_manifest_dir "${manifest_file}" DIRECTORY)
  file(STRINGS "${manifest_file}" _lines)
  set(_count 0)

  foreach(_line IN LISTS _lines)
    if(_line STREQUAL "")
      continue()
    endif()
    if(NOT _line MATCHES "^([0-9a-fA-F]+)  (.+)$")
      message(FATAL_ERROR "tugmcp: malformed manifest line in ${manifest_file}: ${_line}")
    endif()
    set(_expected "${CMAKE_MATCH_1}")
    set(_relative "${CMAKE_MATCH_2}")
    string(TOLOWER "${_expected}" _expected)
    set(_absolute "${_manifest_dir}/${_relative}")
    if(NOT EXISTS "${_absolute}")
      message(FATAL_ERROR "tugmcp: vendored file is missing: ${_relative}")
    endif()
    file(SHA256 "${_absolute}" _actual)
    string(TOLOWER "${_actual}" _actual)
    if(NOT _actual STREQUAL _expected)
      message(FATAL_ERROR
        "tugmcp: SHA256 mismatch for ${_relative}\n"
        "  expected: ${_expected}\n"
        "  actual:   ${_actual}")
    endif()
    math(EXPR _count "${_count} + 1")
  endforeach()

  message(STATUS "tugmcp: verified ${_count} files in ${manifest_file}")
endfunction()
