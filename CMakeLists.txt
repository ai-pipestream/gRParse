cmake_minimum_required(VERSION 3.20)
project(gRParse VERSION 0.1.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_POSITION_INDEPENDENT_CODE ON)

include(FetchContent)

# RapidOcrOnnx 1.2.3 is the maintained C++ RapidOCR implementation.  Its
# upstream CMake project expects locally bundled dependencies, so this project
# builds the same sources against the pinned CUDA ONNX Runtime below.
FetchContent_Declare(rapidocr SOURCE_SUBDIR disabled
  GIT_REPOSITORY https://github.com/RapidAI/RapidOcrOnnx.git
  GIT_TAG 9de89a6934cd962b80db6ab8ad5627e06a709764)
FetchContent_MakeAvailable(rapidocr)

FetchContent_Declare(onnxruntime
  URL https://github.com/microsoft/onnxruntime/releases/download/v1.26.0/onnxruntime-linux-x64-gpu_cuda13-1.26.0.tgz)
FetchContent_MakeAvailable(onnxruntime)

add_library(onnxruntime SHARED IMPORTED GLOBAL)
set_target_properties(onnxruntime PROPERTIES
  IMPORTED_LOCATION "${onnxruntime_SOURCE_DIR}/lib/libonnxruntime.so"
  INTERFACE_INCLUDE_DIRECTORIES "${onnxruntime_SOURCE_DIR}/include")

find_package(OpenCV 4.5 REQUIRED COMPONENTS core imgcodecs imgproc)
find_package(PkgConfig REQUIRED)
pkg_check_modules(POPPLER_CPP REQUIRED IMPORTED_TARGET poppler-cpp)

file(GLOB RAPIDOCR_SOURCES CONFIGURE_DEPENDS "${rapidocr_SOURCE_DIR}/src/*.cpp")
list(REMOVE_ITEM RAPIDOCR_SOURCES "${rapidocr_SOURCE_DIR}/src/main.cpp")
add_library(rapidocr STATIC ${RAPIDOCR_SOURCES})
target_compile_definitions(rapidocr PRIVATE __CUDA__ UNICODE _UNICODE)
target_include_directories(rapidocr
  PUBLIC
    "${rapidocr_SOURCE_DIR}/include"
    "${CMAKE_CURRENT_SOURCE_DIR}/compat")
target_link_libraries(rapidocr PUBLIC onnxruntime ${OpenCV_LIBS})

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
configure_file(docling.proto
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
  DEPENDS docling.proto docling_serve_types.proto docling_serve.proto docling_serve_stream.proto
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

add_executable(grparse-server
  src/main.cpp
  src/document_parser_service.cpp
  src/ocr_engine.cpp)
target_include_directories(grparse-server PRIVATE include)
target_link_libraries(grparse-server PRIVATE docling_proto rapidocr PkgConfig::POPPLER_CPP)

add_executable(grparse-stream-client src/stream_pdf_client.cpp)
target_link_libraries(grparse-stream-client PRIVATE docling_proto)

install(TARGETS grparse-server grparse-stream-client RUNTIME DESTINATION bin)
