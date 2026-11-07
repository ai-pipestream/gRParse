# cmake -DGRPARSE_BINARY_DIR=<build> -DCTEST_COMMAND=<ctest> -P tests/ctest_tiers_test.cmake
#
# Every test in the grparse suite carries exactly one tier label
# (cmake/TestTiers.cmake, #244), so a test added without one fails here
# instead of landing in no tier or two. Reads ctest's own JSON listing.
set(tiers unit loopback models emulators stack bench)

execute_process(
  COMMAND "${CTEST_COMMAND}" --test-dir "${GRPARSE_BINARY_DIR}" -N --show-only=json-v1
  OUTPUT_VARIABLE listing
  RESULT_VARIABLE status)
if(NOT status EQUAL 0)
  message(FATAL_ERROR "ctest -N --show-only=json-v1 failed (${status})")
endif()

string(JSON test_count LENGTH "${listing}" tests)
set(problems "")
set(grparse_count 0)
math(EXPR last "${test_count} - 1")
foreach(index RANGE ${last})
  string(JSON name GET "${listing}" tests ${index} name)
  set(labels "")
  string(JSON property_count ERROR_VARIABLE no_properties
    LENGTH "${listing}" tests ${index} properties)
  if(NOT no_properties AND property_count GREATER 0)
    math(EXPR last_property "${property_count} - 1")
    foreach(p RANGE ${last_property})
      string(JSON property_name GET "${listing}" tests ${index} properties ${p} name)
      if(property_name STREQUAL "LABELS")
        string(JSON label_count LENGTH "${listing}" tests ${index} properties ${p} value)
        math(EXPR last_label "${label_count} - 1")
        foreach(l RANGE ${last_label})
          string(JSON label GET "${listing}" tests ${index} properties ${p} value ${l})
          list(APPEND labels "${label}")
        endforeach()
      endif()
    endforeach()
  endif()
  if(NOT "grparse" IN_LIST labels)
    continue()
  endif()
  math(EXPR grparse_count "${grparse_count} + 1")
  set(found "")
  foreach(tier IN LISTS tiers)
    if(tier IN_LIST labels)
      list(APPEND found "${tier}")
    endif()
  endforeach()
  list(LENGTH found found_count)
  if(NOT found_count EQUAL 1)
    string(APPEND problems "\n  ${name}: tier labels [${found}] (labels: ${labels})")
  endif()
endforeach()

if(grparse_count EQUAL 0)
  message(FATAL_ERROR "no grparse tests listed in ${GRPARSE_BINARY_DIR}")
endif()
if(problems)
  message(FATAL_ERROR "every grparse test needs exactly one tier label "
    "(cmake/TestTiers.cmake):${problems}")
endif()
message(STATUS "ctest-tiers: ${grparse_count} grparse tests, one tier each")
