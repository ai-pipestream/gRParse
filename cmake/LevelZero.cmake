# The native table structure decoder for Intel GPUs (src/levelzero/): an
# OpenCL C kernel compiled to SPIR-V by Intel's offline compiler at build
# time, loaded and launched through the Level Zero loader at run time.  The
# OpenVINO image turns it on; without it an OpenVINO selection refuses the
# table structure model rather than run its decoder anywhere else.
option(GRPARSE_TABLE_LEVEL_ZERO "Build the native Level Zero table structure decoder" OFF)

# The ONNX initializer reader builds everywhere so its test runs in every flavor.
target_sources(grparse_core PRIVATE src/levelzero/onnx_initializers.cpp)
target_include_directories(grparse_core PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/src")

if(GRPARSE_TABLE_LEVEL_ZERO)
  find_path(GRPARSE_LEVEL_ZERO_INCLUDE level_zero/ze_api.h REQUIRED)
  find_library(GRPARSE_LEVEL_ZERO_LIBRARY ze_loader REQUIRED)
  find_program(GRPARSE_OCLOC ocloc REQUIRED)
  set(spirv "${CMAKE_CURRENT_BINARY_DIR}/levelzero/slanet_decoder.spv")
  set(embedded "${CMAKE_CURRENT_BINARY_DIR}/levelzero/slanet_decoder_spirv.cpp")
  set(kernel "${CMAKE_CURRENT_SOURCE_DIR}/src/levelzero/slanet_decoder.cl")
  # SPIR-V, not a device binary: the driver finalizes it for whichever Intel
  # GPU the process opens.  -device only selects ocloc's front end.
  add_custom_command(OUTPUT "${spirv}"
    COMMAND ${CMAKE_COMMAND} -E make_directory "${CMAKE_CURRENT_BINARY_DIR}/levelzero"
    COMMAND "${GRPARSE_OCLOC}" compile -file "${kernel}" -device bmg -spv_only
      -options "-cl-std=CL3.0" -output slanet_decoder -output_no_suffix
      -out_dir "${CMAKE_CURRENT_BINARY_DIR}/levelzero" -q
    DEPENDS "${kernel}"
    COMMENT "Compiling the table structure decoder kernel to SPIR-V")
  add_custom_command(OUTPUT "${embedded}"
    COMMAND ${CMAKE_COMMAND} -DINPUT=${spirv} -DOUTPUT=${embedded}
      -P "${CMAKE_CURRENT_SOURCE_DIR}/cmake/EmbedSpirv.cmake"
    DEPENDS "${spirv}" "${CMAKE_CURRENT_SOURCE_DIR}/cmake/EmbedSpirv.cmake")
  target_sources(grparse_core PRIVATE src/levelzero/table_decoder.cpp "${embedded}")
  target_compile_definitions(grparse_core PRIVATE GRPARSE_TABLE_LEVEL_ZERO=1)
  target_include_directories(grparse_core SYSTEM PRIVATE "${GRPARSE_LEVEL_ZERO_INCLUDE}")
  target_link_libraries(grparse_core PRIVATE "${GRPARSE_LEVEL_ZERO_LIBRARY}")
endif()

if(BUILD_TESTING)
  add_executable(onnx-initializers-test tests/onnx_initializers_test.cpp)
  target_include_directories(onnx-initializers-test PRIVATE
    "${CMAKE_CURRENT_SOURCE_DIR}/tests" "${CMAKE_CURRENT_SOURCE_DIR}/src")
  target_link_libraries(onnx-initializers-test PRIVATE grparse_core grparse_flags)
  add_test(NAME onnx-initializers-test COMMAND onnx-initializers-test)
  set_tests_properties(onnx-initializers-test PROPERTIES LABELS grparse TIMEOUT ${GRPARSE_TEST_TIMEOUT})
  add_dependencies(grparse-tests onnx-initializers-test)
  if(GRPARSE_TABLE_LEVEL_ZERO)
    # The GPU path against the CPU path on real table crops.  Needs the
    # models and an Intel GPU; skips (77) without either, so the image build,
    # which has no GPU, stays green.
    add_executable(level-zero-table-decoder-test tests/level_zero_table_decoder_test.cpp)
    target_include_directories(level-zero-table-decoder-test PRIVATE
      "${CMAKE_CURRENT_SOURCE_DIR}/tests" "${CMAKE_CURRENT_SOURCE_DIR}/src")
    target_link_libraries(level-zero-table-decoder-test PRIVATE grparse_core grparse_flags)
    add_test(NAME level-zero-table-decoder-test COMMAND level-zero-table-decoder-test)
    set_tests_properties(level-zero-table-decoder-test PROPERTIES
      LABELS "grparse;models" TIMEOUT ${GRPARSE_TEST_TIMEOUT} SKIP_RETURN_CODE 77
      ENVIRONMENT
        "GRPARSE_TEST_MODELS_DIR=${CMAKE_CURRENT_SOURCE_DIR}/models;GRPARSE_TEST_DATA_DIR=${CMAKE_CURRENT_SOURCE_DIR}/tests/data")
    add_dependencies(grparse-tests level-zero-table-decoder-test)
  endif()
endif()
