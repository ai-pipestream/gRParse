# Native table structure for Intel GPUs (src/levelzero/): the encoder's and
# decoder's OpenCL C kernels compiled to SPIR-V by Intel's offline compiler
# at build time, loaded and launched through the Level Zero loader at run
# time.  The OpenVINO image turns it on; without it an OpenVINO selection
# refuses the table structure model rather than run it anywhere else.
option(GRPARSE_TABLE_LEVEL_ZERO "Build native Level Zero table structure" OFF)

# The ONNX initializer reader builds everywhere so its test runs in every flavor.
target_sources(grparse_ort_ep PRIVATE src/levelzero/onnx_initializers.cpp)
target_include_directories(grparse_ort_ep PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/src")
target_include_directories(grparse_core PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/src")

if(GRPARSE_TABLE_LEVEL_ZERO)
  find_path(GRPARSE_LEVEL_ZERO_INCLUDE level_zero/ze_api.h REQUIRED)
  find_library(GRPARSE_LEVEL_ZERO_LIBRARY ze_loader REQUIRED)
  find_program(GRPARSE_OCLOC ocloc REQUIRED)
  # Each kernel file to SPIR-V, not a device binary: the driver finalizes it
  # for whichever Intel GPU the process opens (-device only selects ocloc's
  # front end), embedded as a byte array.
  set(embedded_kernels)
  foreach(pair IN ITEMS "slanet_decoder:kSlanetDecoderSpirv" "table_encoder:kTableEncoderSpirv")
    string(REPLACE ":" ";" pair "${pair}")
    list(GET pair 0 name)
    list(GET pair 1 symbol)
    set(source "${CMAKE_CURRENT_SOURCE_DIR}/src/levelzero/${name}.cl")
    set(spirv "${CMAKE_CURRENT_BINARY_DIR}/levelzero/${name}.spv")
    set(embedded "${CMAKE_CURRENT_BINARY_DIR}/levelzero/${name}_spirv.cpp")
    add_custom_command(OUTPUT "${spirv}"
      COMMAND ${CMAKE_COMMAND} -E make_directory "${CMAKE_CURRENT_BINARY_DIR}/levelzero"
      COMMAND "${GRPARSE_OCLOC}" compile -file "${source}" -device bmg -spv_only
        -options "-cl-std=CL3.0" -output ${name} -output_no_suffix
        -out_dir "${CMAKE_CURRENT_BINARY_DIR}/levelzero" -q
      DEPENDS "${source}"
      COMMENT "Compiling ${name}.cl to SPIR-V")
    add_custom_command(OUTPUT "${embedded}"
      COMMAND ${CMAKE_COMMAND} -DINPUT=${spirv} -DOUTPUT=${embedded} -DSYMBOL=${symbol}
        -P "${CMAKE_CURRENT_SOURCE_DIR}/cmake/EmbedSpirv.cmake"
      DEPENDS "${spirv}" "${CMAKE_CURRENT_SOURCE_DIR}/cmake/EmbedSpirv.cmake")
    list(APPEND embedded_kernels "${embedded}")
  endforeach()
  # The graph runtime's kernels: kernels_real.h, graph_kernels.cl, then each
  # kernels_*.cl in name order, joined into one source and compiled for
  # float activations and again with -DGRAPH_FP16.
  file(GLOB graph_kernel_parts CONFIGURE_DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/src/levelzero/kernels_*.cl")
  list(SORT graph_kernel_parts)
  set(graph_kernel_sources "${CMAKE_CURRENT_SOURCE_DIR}/src/levelzero/kernels_real.h"
    "${CMAKE_CURRENT_SOURCE_DIR}/src/levelzero/graph_kernels.cl" ${graph_kernel_parts})
  set(graph_source "${CMAKE_CURRENT_BINARY_DIR}/levelzero/graph_all.cl")
  add_custom_command(OUTPUT "${graph_source}"
    COMMAND ${CMAKE_COMMAND} -E make_directory "${CMAKE_CURRENT_BINARY_DIR}/levelzero"
    COMMAND ${CMAKE_COMMAND} -E cat ${graph_kernel_sources} > "${graph_source}"
    DEPENDS ${graph_kernel_sources}
    COMMENT "Joining the graph runtime's kernel files")
  foreach(flavor IN ITEMS "graph_kernels|kGraphKernelsSpirv|" "graph_kernels_fp16|kGraphKernelsFp16Spirv| -DGRAPH_FP16")
    string(REPLACE "|" ";" flavor "${flavor}")
    list(GET flavor 0 name)
    list(GET flavor 1 symbol)
    list(GET flavor 2 defines)
    set(spirv "${CMAKE_CURRENT_BINARY_DIR}/levelzero/${name}.spv")
    set(embedded "${CMAKE_CURRENT_BINARY_DIR}/levelzero/${name}_spirv.cpp")
    add_custom_command(OUTPUT "${spirv}"
      COMMAND "${GRPARSE_OCLOC}" compile -file "${graph_source}" -device bmg -spv_only
        -options "-cl-std=CL3.0${defines}" -output ${name} -output_no_suffix
        -out_dir "${CMAKE_CURRENT_BINARY_DIR}/levelzero" -q
      DEPENDS "${graph_source}"
      COMMENT "Compiling the graph kernels to SPIR-V (${name})")
    add_custom_command(OUTPUT "${embedded}"
      COMMAND ${CMAKE_COMMAND} -DINPUT=${spirv} -DOUTPUT=${embedded} -DSYMBOL=${symbol}
        -P "${CMAKE_CURRENT_SOURCE_DIR}/cmake/EmbedSpirv.cmake"
      DEPENDS "${spirv}" "${CMAKE_CURRENT_SOURCE_DIR}/cmake/EmbedSpirv.cmake")
    list(APPEND embedded_graph_kernels "${embedded}")
  endforeach()
  # The device and the graph runtime sit in the model-interface library,
  # under make_rapidocr_model (src/inference_model.cpp); table structure in
  # the core library uses the same device.
  target_sources(grparse_ort_ep PRIVATE src/levelzero/device.cpp src/levelzero/graph_ir.cpp src/levelzero/graph_fuse.cpp
    src/levelzero/graph_runtime.cpp ${embedded_graph_kernels})
  target_compile_definitions(grparse_ort_ep PRIVATE GRPARSE_TABLE_LEVEL_ZERO=1)
  target_include_directories(grparse_ort_ep SYSTEM PRIVATE "${GRPARSE_LEVEL_ZERO_INCLUDE}")
  target_link_libraries(grparse_ort_ep PRIVATE "${GRPARSE_LEVEL_ZERO_LIBRARY}")
  target_sources(grparse_core PRIVATE src/levelzero/encoder_plan.cpp src/levelzero/table_encoder.cpp
    src/levelzero/table_gpu.cpp ${embedded_kernels})
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
    # The OCR nets on the graph runtime against ONNX Runtime on CPU, same gate.
    add_executable(level-zero-ocr-test tests/level_zero_ocr_test.cpp)
    target_include_directories(level-zero-ocr-test PRIVATE
      "${CMAKE_CURRENT_SOURCE_DIR}/tests" "${CMAKE_CURRENT_SOURCE_DIR}/src")
    target_include_directories(level-zero-ocr-test SYSTEM PRIVATE "${GRPARSE_LEVEL_ZERO_INCLUDE}")
    target_link_libraries(level-zero-ocr-test PRIVATE grparse_core grparse_ort_ep grparse_flags)
    add_test(NAME level-zero-ocr-test COMMAND level-zero-ocr-test)
    set_tests_properties(level-zero-ocr-test PROPERTIES
      LABELS "grparse;models" TIMEOUT ${GRPARSE_TEST_TIMEOUT} SKIP_RETURN_CODE 77
      ENVIRONMENT
        "GRPARSE_TEST_MODELS_DIR=${CMAKE_CURRENT_SOURCE_DIR}/models;GRPARSE_TEST_DATA_DIR=${CMAKE_CURRENT_SOURCE_DIR}/tests/data")
    add_dependencies(grparse-tests level-zero-ocr-test)
  endif()
endif()
