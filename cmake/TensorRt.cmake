# Native TensorRT for the image models (OCR detection, direction and
# recognition, layout, table structure, figure classes) under a CUDA
# selection: src/tensorrt/ behind grparse::InferenceModel
# (compat/grparse_inference_model.h). ON in the CUDA image only. With it, a
# CUDA selection never creates an ONNX Runtime session for one of these
# models, and a model that will not build as an engine fails startup.
option(GRPARSE_NATIVE_TENSORRT "Run the image models on native TensorRT under a CUDA selection" OFF)

if(GRPARSE_NATIVE_TENSORRT)
  find_package(CUDAToolkit REQUIRED)
  find_path(GRPARSE_TENSORRT_INCLUDE NvInfer.h REQUIRED)
  find_library(GRPARSE_TENSORRT_LIBRARY nvinfer REQUIRED)
  find_library(GRPARSE_TENSORRT_ONNX_LIBRARY nvonnxparser REQUIRED)
  target_sources(grparse_ort_ep PRIVATE
    src/tensorrt/engine_builder.cpp
    src/tensorrt/tensorrt_model.cpp
    src/tensorrt/trt_common.cpp)
  target_compile_definitions(grparse_ort_ep PRIVATE GRPARSE_NATIVE_TENSORRT=1)
  target_include_directories(grparse_ort_ep SYSTEM PRIVATE "${GRPARSE_TENSORRT_INCLUDE}")
  target_link_libraries(grparse_ort_ep PRIVATE
    "${GRPARSE_TENSORRT_LIBRARY}" "${GRPARSE_TENSORRT_ONNX_LIBRARY}" CUDA::cudart)
endif()

# The check the krick GPU suite runs inside the shipped image
# (scripts/test/gpu-placement-cuda-test.sh): every model file as a TensorRT
# engine against the ONNX Runtime CPU reference. It needs a GPU and the
# model files, so inside an image build it only skips (77, tier `models`).
# Registered only here: a flavor without TensorRT has nothing to check, and
# a skip there would be one no tier allows.
if(GRPARSE_NATIVE_TENSORRT AND BUILD_TESTING)
  add_executable(tensorrt-model-test tests/tensorrt_model_test.cpp)
  target_include_directories(tensorrt-model-test PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/tests")
  target_link_libraries(tensorrt-model-test PRIVATE grparse_core grparse_ort_ep grparse_flags)
  add_test(NAME tensorrt-model-test COMMAND tensorrt-model-test)
  set_tests_properties(tensorrt-model-test PROPERTIES
    LABELS grparse TIMEOUT 1800 SKIP_RETURN_CODE 77)
  add_dependencies(grparse-tests tensorrt-model-test)
endif()
