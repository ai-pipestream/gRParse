# Test tiers (#244). Every test of the grparse suite carries `grparse` (it
# keeps `ctest -L grparse` clear of the tests fetched dependencies register)
# plus exactly one tier label that says what it needs to run:
#
#   unit       pure functions on fixtures; well under a second each
#   loopback   an in-process gRPC server, fake backend or loopback HTTP
#   models     the model files under models/ (skip 77 without them, a
#              failure under GRPARSE_TEST_REQUIRE_MODELS=1)
#   embeddings the embedding model bundle (GRPARSE_EMBEDDING_TEST_MODELS);
#              skip 77 inside every image build, run after the openvino build
#              through the shipped grparse-embedding-check binaries
#   emulators  storage and VLM emulators the cpu-image CI job starts after
#              the build (skip 77 inside the image build)
#   stack      a docker compose stack; opt-in through GRPARSE_STACK_SUITES
#              (scripts/test/stack-suite.sh), skip 77 otherwise
#   bench      timing benchmarks: grparse-bench (#251), skip 77 unless
#              GRPARSE_BENCH_ENFORCE=1 gates it on tests/bench/baseline.textproto
#
# The image builds run `-L grparse -LE "stack|eval|bench"`; CMakePresets.json
# names the same filters (pr, models, nightly, release), and README
# "Development" has the table of what runs where. ctest-tiers (below) fails
# when a test has no tier or two, so a new test has to pick one.
# The eval tier (vlm-oracle-eval, structural-scorecard) keeps its own label
# without `grparse`, as before.
if(NOT BUILD_TESTING)
  return()
endif()

set(GRPARSE_TIER_LOOPBACK
  streaming-service-test document-collectors-test vlm-convert-test
  chart-derender-test remote-pdf-source-test pdf-source-test
  remote-sources-test storage-connectors-test consensus-page-source-test
  prometheus-metrics-test page-scheduler-test page-previews-test
  ort-session-ep-test)
set(GRPARSE_TIER_MODELS
  layout-engine-test table-structure-engine-test figure-classifier-test
  chunk-hf-model-test)
set(GRPARSE_TIER_EMBEDDINGS embedding-model-test embedding-reference-test)
set(GRPARSE_TIER_EMULATORS
  storage-connectors-e2e-test remote-sources-e2e-test chart-outputs-e2e-test)

# The compose-stack suites, registered so `ctest -L stack` lists and runs
# them. They run exactly as they do by hand (scripts/test/stack-suite.sh
# only decides whether to); each needs its stack and is skipped (77) unless
# GRPARSE_STACK_SUITES names it or says `all`.
set(GRPARSE_TIER_STACK
  stack-e2e s3-corpus-eval ocr-presets-e2e embedding-rpc
  compose-preflight observability-compose)
foreach(suite IN LISTS GRPARSE_TIER_STACK)
  add_test(NAME ${suite}
    COMMAND sh ${CMAKE_SOURCE_DIR}/scripts/test/stack-suite.sh ${suite}
    WORKING_DIRECTORY ${CMAKE_SOURCE_DIR})
  set_tests_properties(${suite} PROPERTIES
    LABELS "grparse;stack" SKIP_RETURN_CODE 77 TIMEOUT 3600)
endforeach()

set(GRPARSE_TIER_BENCH grparse-bench)

foreach(tier loopback models embeddings emulators bench)
  string(TOUPPER "${tier}" tier_var)
  foreach(test_name IN LISTS GRPARSE_TIER_${tier_var})
    if(NOT TEST ${test_name})
      continue()  # built only for some flavors (embedding-reference-test)
    endif()
    set_property(TEST ${test_name} APPEND PROPERTY LABELS ${tier})
  endforeach()
endforeach()

# The checker runs ctest -N over this tree and fails on a grparse test with
# no tier label or more than one. It is a unit test itself.
add_test(NAME ctest-tiers
  COMMAND ${CMAKE_COMMAND} -DGRPARSE_BINARY_DIR=${CMAKE_BINARY_DIR}
    -DCTEST_COMMAND=${CMAKE_CTEST_COMMAND}
    -P ${CMAKE_SOURCE_DIR}/tests/ctest_tiers_test.cmake)
set_tests_properties(ctest-tiers PROPERTIES LABELS grparse TIMEOUT 60)

# Everything else in the grparse suite is a unit test.
get_property(grparse_all_tests DIRECTORY PROPERTY TESTS)
set(grparse_tier_names loopback models embeddings emulators stack bench)
foreach(test_name IN LISTS grparse_all_tests)
  get_test_property(${test_name} LABELS test_labels)
  if(NOT test_labels OR NOT "grparse" IN_LIST test_labels)
    continue()
  endif()
  set(has_tier FALSE)
  foreach(tier IN LISTS grparse_tier_names)
    if(tier IN_LIST test_labels)
      set(has_tier TRUE)
    endif()
  endforeach()
  if(NOT has_tier)
    set_property(TEST ${test_name} APPEND PROPERTY LABELS unit)
  endif()
endforeach()
