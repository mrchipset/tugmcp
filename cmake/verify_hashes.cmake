# Verify the SHA256 of vendored third-party files.
# Included from the top-level CMakeLists.txt when TUGMCP_VERIFY_3RD_HASHES=ON.

set(_tugmcp_root "${CMAKE_CURRENT_LIST_DIR}/..")

function(_tugmcp_verify_hash rel_path expected)
  set(abs_path "${_tugmcp_root}/${rel_path}")
  if(NOT EXISTS "${abs_path}")
    message(FATAL_ERROR "tugmcp: vendored dependency file is missing: ${rel_path}")
  endif()
  file(SHA256 "${abs_path}" actual)
  if(NOT actual STREQUAL expected)
    message(FATAL_ERROR
      "tugmcp: SHA256 mismatch for ${rel_path}\n"
      "  expected: ${expected}\n"
      "  actual:   ${actual}")
  endif()
endfunction()

_tugmcp_verify_hash("3rd/doctest/doctest/doctest.h"
  "cfd518a3ef90f67e1f3ba514df23fb3627437de1a2feeba78cf5062a40021421")
_tugmcp_verify_hash("3rd/doctest/scripts/cmake/doctest.cmake"
  "6a60f778f86958c23d308d532356b37ec6e735137be7adf02cb17d688b559711")
_tugmcp_verify_hash("3rd/doctest/scripts/cmake/doctestAddTests.cmake"
  "3ec2839f12b7a12b0d7ef6244b60eecae40ad797caa3f016fc09875956a518dc")
_tugmcp_verify_hash("3rd/nlohmann_json/nlohmann/json.hpp"
  "aaf127c04cb31c406e5b04a63f1ae89369fccde6d8fa7cdda1ed4f32dfc5de63")
_tugmcp_verify_hash("3rd/nlohmann_json/nlohmann/json_fwd.hpp"
  "fb6aa70cbece087f37ab4685c182b287c53be54f785f981b9db9d30d2d028b37")
_tugmcp_verify_hash("LICENSES/doctest-MIT.txt"
  "0fe0b331fa1513dcce8604ff1fa925f32d1cea17d8aeb1c2471fad40d291adc5")
_tugmcp_verify_hash("LICENSES/nlohmann-json-MIT.txt"
  "46a65cffd1ea955132d95a8dd921640714a8d6b537d2e4e482d31145ae95b603")
_tugmcp_verify_hash("LICENSES/jsoncons-BSL-1.0.txt"
  "97550c71a0bdfc20bf85c6c5167d7b7785f9bd3448baee048fbedf742ed52270")
_tugmcp_verify_hash("LICENSES/valijson-BSD-2-Clause.txt"
  "61b5766db38eaa3f320c366357956d7743424bfbf90fd78f5943866ba237589e")

# Multi-file vendored libraries use per-file manifests.
include("${CMAKE_CURRENT_LIST_DIR}/verify_manifest.cmake")
tugmcp_verify_manifest("${_tugmcp_root}/3rd/jsoncons/MANIFEST.sha256")
tugmcp_verify_manifest("${_tugmcp_root}/3rd/valijson/MANIFEST.sha256")
