# The CPU reference uses the existing ONNX Runtime dependency. GPU engines
# use native vendor APIs directly and are explicitly selected at build time.
option(GRPARSE_EMBED_OPENVINO "Build native Intel embedding inference" OFF)
option(GRPARSE_EMBED_TENSORRT "Build native NVIDIA embedding inference" OFF)

add_executable(grparse-embed-text src/embed_text_client.cpp)
target_link_libraries(grparse-embed-text PRIVATE grparse_core grparse_flags)
install(TARGETS grparse-embed-text RUNTIME DESTINATION bin)

target_sources(grparse_core PRIVATE
  src/embedding.cpp
  src/embeddings/engine.cpp
  src/embeddings/cpu_backend.cpp)

if(GRPARSE_EMBED_OPENVINO)
  # Do not import the wheel's C++ target: its _GLIBCXX_USE_CXX11_ABI=0
  # definition would change the ABI of grparse_core and break gRPC linkage.
  if(DEFINED GRPARSE_OPENVINO_ROOT AND NOT "${GRPARSE_OPENVINO_ROOT}" STREQUAL "")
    # Discard prior discovery so reconfiguring with a different root cannot
    # silently retain system headers or a library from the previous SDK.
    unset(GRPARSE_OPENVINO_INCLUDE CACHE)
    unset(GRPARSE_OPENVINO_INCLUDE)
    unset(GRPARSE_OPENVINO_C_LIBRARY CACHE)
    unset(GRPARSE_OPENVINO_C_LIBRARY)
    find_path(GRPARSE_OPENVINO_INCLUDE openvino/c/openvino.h
      PATHS "${GRPARSE_OPENVINO_ROOT}/include" NO_DEFAULT_PATH REQUIRED)
    find_library(GRPARSE_OPENVINO_C_LIBRARY NAMES openvino_c libopenvino_c.so.2541
      PATHS "${GRPARSE_OPENVINO_ROOT}/libs" NO_DEFAULT_PATH REQUIRED)
  else()
    find_path(GRPARSE_OPENVINO_INCLUDE openvino/c/openvino.h REQUIRED)
    find_library(GRPARSE_OPENVINO_C_LIBRARY NAMES openvino_c libopenvino_c.so.2541 REQUIRED)
  endif()
  target_sources(grparse_core PRIVATE src/embeddings/intel_backend.cpp)
  target_compile_definitions(grparse_core PRIVATE GRPARSE_EMBED_OPENVINO=1)
  target_include_directories(grparse_core SYSTEM PRIVATE "${GRPARSE_OPENVINO_INCLUDE}")
  target_link_libraries(grparse_core PRIVATE "${GRPARSE_OPENVINO_C_LIBRARY}")
endif()

# Independent reference owns its tokenizer configuration, native session and
# pooling. Keep this separate from the production-factory model/parity test.
if(BUILD_TESTING)
  add_executable(embedding-reference-test tests/embedding_reference_test.cpp)
  target_include_directories(embedding-reference-test PRIVATE
    "${CMAKE_CURRENT_SOURCE_DIR}/tests" "${tokenizers_cpp_SOURCE_DIR}/include")
  target_link_libraries(embedding-reference-test PRIVATE
    grparse_core grparse_flags onnxruntime tokenizers_cpp)
  if(GRPARSE_EMBED_OPENVINO)
    target_compile_definitions(embedding-reference-test PRIVATE GRPARSE_REFERENCE_OPENVINO=1)
    target_include_directories(embedding-reference-test SYSTEM PRIVATE "${GRPARSE_OPENVINO_INCLUDE}")
    target_link_libraries(embedding-reference-test PRIVATE "${GRPARSE_OPENVINO_C_LIBRARY}")
  endif()
  add_test(NAME embedding-reference-test COMMAND embedding-reference-test)
  set_tests_properties(embedding-reference-test PROPERTIES
    LABELS "grparse;embedding-reference" TIMEOUT ${GRPARSE_TEST_TIMEOUT} SKIP_RETURN_CODE 77)
  add_dependencies(grparse-tests embedding-reference-test)
endif()

if(GRPARSE_EMBED_TENSORRT)
  find_package(CUDAToolkit REQUIRED)
  find_path(GRPARSE_TENSORRT_INCLUDE NvInfer.h REQUIRED)
  find_library(GRPARSE_TENSORRT_LIBRARY nvinfer REQUIRED)
  find_library(GRPARSE_TENSORRT_ONNX_LIBRARY nvonnxparser REQUIRED)
  target_sources(grparse_core PRIVATE src/embeddings/nvidia_backend.cpp)
  target_compile_definitions(grparse_core PRIVATE GRPARSE_EMBED_TENSORRT=1)
  target_include_directories(grparse_core SYSTEM PRIVATE "${GRPARSE_TENSORRT_INCLUDE}")
  target_link_libraries(grparse_core PRIVATE
    "${GRPARSE_TENSORRT_LIBRARY}" "${GRPARSE_TENSORRT_ONNX_LIBRARY}" CUDA::cudart)
endif()

if(BUILD_TESTING)
  add_executable(embedding-test tests/embedding_test.cpp)
  target_include_directories(embedding-test PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/tests")
  target_link_libraries(embedding-test PRIVATE grparse_core grparse_flags)
  add_test(NAME embedding-test COMMAND embedding-test)
  set_tests_properties(embedding-test PROPERTIES LABELS grparse TIMEOUT ${GRPARSE_TEST_TIMEOUT})
  add_dependencies(grparse-tests embedding-test)
  add_executable(embedding-model-test tests/embedding_model_test.cpp)
  target_include_directories(embedding-model-test PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/tests")
  target_link_libraries(embedding-model-test PRIVATE grparse_core grparse_flags)
  add_test(NAME embedding-model-test COMMAND embedding-model-test)
  set_tests_properties(embedding-model-test PROPERTIES
    LABELS "grparse;embedding-model" TIMEOUT ${GRPARSE_TEST_TIMEOUT} SKIP_RETURN_CODE 77)
  add_dependencies(grparse-tests embedding-model-test)
endif()
