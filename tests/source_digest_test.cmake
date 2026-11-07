# cmake -DGRPARSE_SOURCE_ROOT=<repo> -DSCRATCH=<dir> -P tests/source_digest_test.cmake
#
# The build digest (cmake/SourceDigest.cmake) must be stable for one tree and
# move when any identity source changes, or Document.parse.build stops
# telling two builds apart (#239). Runs the real function over scratch trees.
include("${GRPARSE_SOURCE_ROOT}/cmake/SourceDigest.cmake")

file(REMOVE_RECURSE "${SCRATCH}")
foreach(tree a b)
  file(WRITE "${SCRATCH}/${tree}/CMakeLists.txt" "project(x)\n")
  file(WRITE "${SCRATCH}/${tree}/src/parse.cpp" "int parse() { return 1; }\n")
  file(WRITE "${SCRATCH}/${tree}/include/grparse/parse.h" "int parse();\n")
  file(WRITE "${SCRATCH}/${tree}/document.proto" "syntax = \"proto3\";\n")
  # Not an identity source: docs never move the digest.
  file(WRITE "${SCRATCH}/${tree}/docs/notes.md" "${tree}\n")
endforeach()

grparse_source_digest(digest_a "${SCRATCH}/a")
grparse_source_digest(digest_b "${SCRATCH}/b")
if(NOT digest_a MATCHES "^[0-9a-f]+$")
  message(FATAL_ERROR "digest is not lowercase hex: '${digest_a}'")
endif()
string(LENGTH "${digest_a}" digest_length)
if(NOT digest_length EQUAL 64)
  message(FATAL_ERROR "digest is not a SHA-256 (${digest_length} chars)")
endif()
if(NOT digest_a STREQUAL digest_b)
  message(FATAL_ERROR "identical identity sources gave two digests; docs moved it")
endif()

foreach(changed src/parse.cpp include/grparse/parse.h document.proto CMakeLists.txt)
  file(READ "${SCRATCH}/a/${changed}" original)
  file(APPEND "${SCRATCH}/b/${changed}" "// one more line\n")
  grparse_source_digest(digest_changed "${SCRATCH}/b")
  if(digest_changed STREQUAL digest_a)
    message(FATAL_ERROR "changing ${changed} left the build digest unchanged")
  endif()
  file(WRITE "${SCRATCH}/b/${changed}" "${original}")
endforeach()

# A renamed file with the same bytes is a different tree too.
file(RENAME "${SCRATCH}/b/src/parse.cpp" "${SCRATCH}/b/src/parse2.cpp")
grparse_source_digest(digest_renamed "${SCRATCH}/b")
if(digest_renamed STREQUAL digest_a)
  message(FATAL_ERROR "renaming a source left the build digest unchanged")
endif()

file(REMOVE_RECURSE "${SCRATCH}")
message(STATUS "source-digest-script: ok")
