cmake_minimum_required(VERSION 3.20)
project(gRParse VERSION 0.1.0 LANGUAGES CXX)

include(CTest)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_POSITION_INDEPENDENT_CODE ON)

option(GRPARSE_WERROR "Treat warnings in gRParse code as errors" OFF)
set(GRPARSE_SANITIZE "" CACHE STRING
  "Sanitizers for gRParse targets: address, thread, undefined (comma separated)")

# Applied to first-party targets only; fetched dependencies keep their own flags.
# gRPC pulls its own and abseil's headers in with -I rather than -isystem, so
# any warning they trip is reported against our translation units.  -Wpedantic
# (abseil's __int128), -Wshadow, and deprecation notices therefore stay off or
# non-fatal; everything -Wall/-Wextra covers is an error under GRPARSE_WERROR.
add_library(grparse_flags INTERFACE)
target_compile_options(grparse_flags INTERFACE -Wall -Wextra -Wnon-virtual-dtor)
if(GRPARSE_WERROR)
  target_compile_options(grparse_flags INTERFACE -Werror -Wno-error=deprecated-declarations)
endif()
if(GRPARSE_SANITIZE)
  target_compile_options(grparse_flags INTERFACE
    -fsanitize=${GRPARSE_SANITIZE} -fno-omit-frame-pointer -g)
  target_link_options(grparse_flags INTERFACE -fsanitize=${GRPARSE_SANITIZE})
endif()

include(FetchContent)

# RapidOcrOnnx 1.2.3 is the maintained C++ RapidOCR implementation.  Its
# upstream CMake project expects locally bundled dependencies, so this project
# builds the same sources against the pinned ONNX Runtime below.  The patch
# reroutes each net's execution-provider setup through grparse_session_ep.h so
# CUDA, OpenVINO, and CPU share one decision point; the server refuses to start
# if the hook never ran (a stale unpatched dependency cache).
FetchContent_Declare(rapidocr SOURCE_SUBDIR disabled
  GIT_REPOSITORY https://github.com/RapidAI/RapidOcrOnnx.git
  GIT_TAG 9de89a6934cd962b80db6ab8ad5627e06a709764
  # checkout -- src first so the step is idempotent: FetchContent re-runs the
  # patch step whenever the subbuild re-executes, including on cached trees
  # that are already patched.
  PATCH_COMMAND git checkout -- src
        COMMAND git apply "${CMAKE_CURRENT_SOURCE_DIR}/patches/rapidocr-session-ep.patch")
FetchContent_MakeAvailable(rapidocr)
# FetchContent only patches when it populates, so a dependency tree cached
# before the patch existed would build the upstream provider wiring and the
# configured EP could silently degrade.  Fail the configure instead.
foreach(patched_net DbNet AngleNet CrnnNet)
  file(READ "${rapidocr_SOURCE_DIR}/src/${patched_net}.cpp" GRPARSE_NET_SOURCE)
  string(FIND "${GRPARSE_NET_SOURCE}" "grparse::append_execution_provider" GRPARSE_HOOK_AT)
  if(GRPARSE_HOOK_AT EQUAL -1)
    message(FATAL_ERROR "${patched_net}.cpp is unpatched: the rapidocr source tree predates "
      "patches/rapidocr-session-ep.patch. Delete _deps/rapidocr-* from the build directory "
      "(or the docker build cache) and re-run CMake.")
  endif()
endforeach()
unset(GRPARSE_NET_SOURCE)

# Which ONNX Runtime binary package to link.  `cuda` is Microsoft's NVIDIA GPU
# release.  `openvino` takes headers from the matching official release and
# libraries from Intel's PyPI wheel — the only prebuilt Linux distribution of
# the OpenVINO execution provider — which bundles OpenVINO 2025.4.1 and its
# Intel GPU/CPU/NPU plugins.
set(GRPARSE_ORT_PACKAGE "cuda" CACHE STRING
  "ONNX Runtime package to link: cuda or openvino")
if(GRPARSE_ORT_PACKAGE STREQUAL "cuda")
  FetchContent_Declare(onnxruntime
    URL https://github.com/microsoft/onnxruntime/releases/download/v1.27.1/onnxruntime-linux-x64-gpu_cuda13-1.27.1.tgz
    DOWNLOAD_EXTRACT_TIMESTAMP TRUE)
  FetchContent_MakeAvailable(onnxruntime)
  set(GRPARSE_ORT_LIBRARY "${onnxruntime_SOURCE_DIR}/lib/libonnxruntime.so")
elseif(GRPARSE_ORT_PACKAGE STREQUAL "openvino")
  FetchContent_Declare(onnxruntime
    URL https://github.com/microsoft/onnxruntime/releases/download/v1.24.1/onnxruntime-linux-x64-1.24.1.tgz
    URL_HASH SHA256=9142552248b735920f9390027e4512a2cacf8946a1ffcbe9071a5c210531026f
    DOWNLOAD_EXTRACT_TIMESTAMP TRUE)
  set(GRPARSE_ORTOV_WHEEL_NAME
    "onnxruntime_openvino-1.24.1-cp312-cp312-manylinux_2_28_x86_64.whl")
  FetchContent_Declare(onnxruntime_openvino_wheel
    URL "https://files.pythonhosted.org/packages/50/cf/17ba72de2df0fcba349937d2788f154397bbc2d1a2d67772a97e26f6bc5f/${GRPARSE_ORTOV_WHEEL_NAME}"
    URL_HASH SHA256=d617fac2f59a6ab5ea59a788c3e1592240a129642519aaeaa774761dfe35150e
    DOWNLOAD_NO_EXTRACT TRUE)
  FetchContent_MakeAvailable(onnxruntime onnxruntime_openvino_wheel)
  set(GRPARSE_ORT_OPENVINO_LIB_DIR "${CMAKE_CURRENT_BINARY_DIR}/ort-openvino/onnxruntime/capi")
  if(NOT EXISTS "${GRPARSE_ORT_OPENVINO_LIB_DIR}/libonnxruntime.so.1.24.1")
    file(ARCHIVE_EXTRACT
      INPUT "${onnxruntime_openvino_wheel_SOURCE_DIR}/${GRPARSE_ORTOV_WHEEL_NAME}"
      DESTINATION "${CMAKE_CURRENT_BINARY_DIR}/ort-openvino"
      PATTERNS "onnxruntime/capi/lib*")
  endif()
  # The wheel ships only the fully versioned name; the SONAME is the .so.1.
  file(CREATE_LINK "libonnxruntime.so.1.24.1"
    "${GRPARSE_ORT_OPENVINO_LIB_DIR}/libonnxruntime.so.1" SYMBOLIC)
  set(GRPARSE_ORT_LIBRARY "${GRPARSE_ORT_OPENVINO_LIB_DIR}/libonnxruntime.so.1")
else()
  message(FATAL_ERROR "GRPARSE_ORT_PACKAGE must be cuda or openvino")
endif()

add_library(onnxruntime SHARED IMPORTED GLOBAL)
set_target_properties(onnxruntime PROPERTIES
  IMPORTED_LOCATION "${GRPARSE_ORT_LIBRARY}"
  INTERFACE_INCLUDE_DIRECTORIES "${onnxruntime_SOURCE_DIR}/include")

# ZXing decodes barcode/QR payloads from figure crops (E3).  Readers only:
# the server never generates barcodes.
set(ZXING_READERS ON CACHE BOOL "" FORCE)
set(ZXING_WRITERS OFF CACHE BOOL "" FORCE)
set(ZXING_EXAMPLES OFF CACHE BOOL "" FORCE)
set(ZXING_BLACKBOX_TESTS OFF CACHE BOOL "" FORCE)
set(ZXING_UNIT_TESTS OFF CACHE BOOL "" FORCE)
FetchContent_Declare(zxing
  GIT_REPOSITORY https://github.com/zxing-cpp/zxing-cpp.git
  GIT_TAG v2.3.0
  GIT_SHALLOW TRUE)
FetchContent_MakeAvailable(zxing)

find_package(OpenCV 4.5 REQUIRED COMPONENTS core imgcodecs imgproc)
find_package(PkgConfig REQUIRED)
find_package(Threads REQUIRED)
pkg_check_modules(POPPLER_CPP REQUIRED IMPORTED_TARGET poppler-cpp)

# The execution-provider hook the patched RapidOcrOnnx nets call into.
add_library(grparse_ort_ep STATIC src/ort_session_ep.cpp)
target_include_directories(grparse_ort_ep PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}/compat")
target_link_libraries(grparse_ort_ep PUBLIC onnxruntime PRIVATE grparse_flags)

file(GLOB RAPIDOCR_SOURCES CONFIGURE_DEPENDS "${rapidocr_SOURCE_DIR}/src/*.cpp")
list(REMOVE_ITEM RAPIDOCR_SOURCES "${rapidocr_SOURCE_DIR}/src/main.cpp")
add_library(rapidocr STATIC ${RAPIDOCR_SOURCES})
target_compile_definitions(rapidocr PRIVATE __CUDA__ UNICODE _UNICODE)
target_include_directories(rapidocr
  PUBLIC
    "${rapidocr_SOURCE_DIR}/include"
    "${CMAKE_CURRENT_SOURCE_DIR}/compat")
target_link_libraries(rapidocr PUBLIC onnxruntime ${OpenCV_LIBS} grparse_ort_ep)

set(gRPC_INSTALL OFF CACHE BOOL "" FORCE)
set(gRPC_BUILD_TESTS OFF CACHE BOOL "" FORCE)
set(protobuf_BUILD_TESTS OFF CACHE BOOL "" FORCE)
set(protobuf_INSTALL OFF CACHE BOOL "" FORCE)
set(ABSL_ENABLE_INSTALL OFF CACHE BOOL "" FORCE)
FetchContent_Declare(grpc
  GIT_REPOSITORY https://github.com/grpc/grpc.git
  GIT_TAG v1.82.1
  GIT_SHALLOW TRUE
  GIT_SUBMODULES_RECURSE TRUE)
FetchContent_MakeAvailable(grpc)

set(GENERATED_DIR "${CMAKE_CURRENT_BINARY_DIR}/generated")
set(STAGED_PROTO_DIR "${CMAKE_CURRENT_BINARY_DIR}/proto")
file(MAKE_DIRECTORY "${STAGED_PROTO_DIR}/ai/docling/core/v1")
file(MAKE_DIRECTORY "${STAGED_PROTO_DIR}/ai/docling/serve/v1")
configure_file(docling_document.proto
  "${STAGED_PROTO_DIR}/ai/docling/core/v1/docling_document.proto" COPYONLY)
configure_file(docling_serve_types.proto
  "${STAGED_PROTO_DIR}/ai/docling/serve/v1/docling_serve_types.proto" COPYONLY)
configure_file(docling_serve.proto
  "${STAGED_PROTO_DIR}/ai/docling/serve/v1/docling_serve.proto" COPYONLY)
configure_file(docling_serve_stream.proto
  "${STAGED_PROTO_DIR}/ai/docling/serve/v1/docling_serve_stream.proto" COPYONLY)

set(DOCLING_DOCUMENT_PROTO "${STAGED_PROTO_DIR}/ai/docling/core/v1/docling_document.proto")
set(DOCLING_TYPES_PROTO "${STAGED_PROTO_DIR}/ai/docling/serve/v1/docling_serve_types.proto")
set(DOCLING_SERVICE_PROTO "${STAGED_PROTO_DIR}/ai/docling/serve/v1/docling_serve.proto")
set(DOCLING_STREAM_PROTO "${STAGED_PROTO_DIR}/ai/docling/serve/v1/docling_serve_stream.proto")
add_custom_command(
  OUTPUT "${GENERATED_DIR}/ai/docling/core/v1/docling_document.pb.cc"
         "${GENERATED_DIR}/ai/docling/core/v1/docling_document.pb.h"
         "${GENERATED_DIR}/ai/docling/serve/v1/docling_serve_types.pb.cc"
         "${GENERATED_DIR}/ai/docling/serve/v1/docling_serve_types.pb.h"
         "${GENERATED_DIR}/ai/docling/serve/v1/docling_serve.pb.cc"
         "${GENERATED_DIR}/ai/docling/serve/v1/docling_serve.pb.h"
         "${GENERATED_DIR}/ai/docling/serve/v1/docling_serve.grpc.pb.cc"
         "${GENERATED_DIR}/ai/docling/serve/v1/docling_serve.grpc.pb.h"
         "${GENERATED_DIR}/ai/docling/serve/v1/docling_serve_stream.pb.cc"
         "${GENERATED_DIR}/ai/docling/serve/v1/docling_serve_stream.pb.h"
         "${GENERATED_DIR}/ai/docling/serve/v1/docling_serve_stream.grpc.pb.cc"
         "${GENERATED_DIR}/ai/docling/serve/v1/docling_serve_stream.grpc.pb.h"
  COMMAND $<TARGET_FILE:protoc>
  ARGS --proto_path=${STAGED_PROTO_DIR}
       --proto_path=${protobuf_SOURCE_DIR}/src
       --cpp_out=${GENERATED_DIR}
       --grpc_out=${GENERATED_DIR}
       --plugin=protoc-gen-grpc=$<TARGET_FILE:grpc_cpp_plugin>
       ${DOCLING_DOCUMENT_PROTO} ${DOCLING_TYPES_PROTO} ${DOCLING_SERVICE_PROTO} ${DOCLING_STREAM_PROTO}
  DEPENDS docling_document.proto docling_serve_types.proto docling_serve.proto docling_serve_stream.proto
          protoc grpc_cpp_plugin
  VERBATIM)
add_library(docling_proto
  "${GENERATED_DIR}/ai/docling/core/v1/docling_document.pb.cc"
  "${GENERATED_DIR}/ai/docling/serve/v1/docling_serve_types.pb.cc"
  "${GENERATED_DIR}/ai/docling/serve/v1/docling_serve.pb.cc"
  "${GENERATED_DIR}/ai/docling/serve/v1/docling_serve.grpc.pb.cc"
  "${GENERATED_DIR}/ai/docling/serve/v1/docling_serve_stream.pb.cc"
  "${GENERATED_DIR}/ai/docling/serve/v1/docling_serve_stream.grpc.pb.cc")
target_include_directories(docling_proto PUBLIC "${GENERATED_DIR}")
target_link_libraries(docling_proto PUBLIC grpc++)

add_library(grparse_core
  src/barcode_decoder.cpp
  src/base64.cpp
  src/document_parser_service.cpp
  src/figure_classifier.cpp
  src/document_assembly.cpp
  src/in_memory_document.cpp
  src/ocr_engine.cpp
  src/layout_engine.cpp
  src/page_scheduler.cpp
  src/reading_order.cpp
  src/region_geometry.cpp
  src/table_structure.cpp
  src/table_structure_engine.cpp
  src/text_geometry.cpp)
target_include_directories(grparse_core PUBLIC include)
target_link_libraries(grparse_core
  PUBLIC docling_proto rapidocr PkgConfig::POPPLER_CPP Threads::Threads
  PRIVATE ZXing::ZXing grparse_flags)

add_executable(grparse-server src/main.cpp)
target_link_libraries(grparse-server PRIVATE grparse_core grparse_flags grpc++_reflection)

add_executable(grparse-stream-client src/stream_pdf_client.cpp)
target_link_libraries(grparse-stream-client PRIVATE docling_proto grparse_flags)

if(BUILD_TESTING)
  set(GRPARSE_TESTS
    barcode-decoder-test
    base64-test
    document-assembly-test
    figure-classifier-test
    layout-engine-test
    page-scheduler-test
    pdf-source-test
    reading-order-test
    resource-pool-test
    streaming-service-test
    table-structure-engine-test
    table-structure-test
    text-geometry-test)
  foreach(test_name IN LISTS GRPARSE_TESTS)
    string(REPLACE "-" "_" test_source "${test_name}")
    add_executable(${test_name} "tests/${test_source}.cpp")
    target_link_libraries(${test_name} PRIVATE grparse_core grparse_flags)
    add_test(NAME ${test_name} COMMAND ${test_name})
  endforeach()
  # The scheduler and streaming tests exercise multi-worker paths; keep their
  # ceiling well above the ~1s they need so a stall fails instead of hanging CI.
  # The label keeps `ctest -L grparse` clear of tests registered by fetched
  # dependencies (re2 registers ~20 of its own).
  set_tests_properties(${GRPARSE_TESTS} PROPERTIES TIMEOUT 120 LABELS grparse)
  # The layout golden needs the real model file; without it the test skips
  # (exit 77) so environments that never download models stay green while any
  # environment with models proves the decode against the reference output.
  set_tests_properties(figure-classifier-test layout-engine-test table-structure-engine-test PROPERTIES
    SKIP_RETURN_CODE 77
    ENVIRONMENT
      "GRPARSE_TEST_MODELS_DIR=${CMAKE_CURRENT_SOURCE_DIR}/models;GRPARSE_TEST_DATA_DIR=${CMAKE_CURRENT_SOURCE_DIR}/tests/data")
  # ZXing is compiled in, so the barcode tests never skip; they only need the
  # committed QR fixture.  The scheduler test loads the same fixture to prove
  # the class-triggered decode end to end.
  set_tests_properties(barcode-decoder-test page-scheduler-test PROPERTIES
    ENVIRONMENT "GRPARSE_TEST_DATA_DIR=${CMAKE_CURRENT_SOURCE_DIR}/tests/data")
endif()

install(TARGETS grparse-server grparse-stream-client RUNTIME DESTINATION bin)
