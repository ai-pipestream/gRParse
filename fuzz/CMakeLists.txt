# Standalone libFuzzer project for the in-process ingest door (the code path
# that touches attacker-controlled bytes before any model runs):
#   raster-source-fuzzer - OpenCV imdecode-backed RasterPageSource: open + decode
# PDFs never parse in this process; they go to a PdfBackendService, which
# fuzzes its own engine.
#
# Deliberately NOT part of the top-level build: the door needs only OpenCV
# and the headers under ../include, so this project skips the gRPC/ONNX
# Runtime fetch and configures in seconds.  See fuzz/README.md.
cmake_minimum_required(VERSION 3.20)
project(grparse-fuzz LANGUAGES CXX)

if(NOT CMAKE_CXX_COMPILER_ID MATCHES "Clang")
  message(FATAL_ERROR
    "libFuzzer needs Clang; configure with -DCMAKE_CXX_COMPILER=clang++")
endif()

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

find_package(OpenCV 4.5 REQUIRED COMPONENTS core imgcodecs imgproc)
find_package(Threads REQUIRED)

set(GRPARSE_ROOT "${CMAKE_CURRENT_SOURCE_DIR}/..")

add_library(fuzz_flags INTERFACE)
target_compile_options(fuzz_flags INTERFACE
  -g -O1 -fno-omit-frame-pointer -fsanitize=fuzzer,address,undefined)
target_link_options(fuzz_flags INTERFACE -fsanitize=fuzzer,address,undefined)
# Compile out the gRPC-backed PDF dispatch: a fuzzer must never dial a
# backend, and this project deliberately skips the gRPC fetch.
target_compile_definitions(fuzz_flags INTERFACE GRPARSE_FUZZ_STANDALONE)

add_executable(raster-source-fuzzer
  raster_source_fuzzer.cpp
  "${GRPARSE_ROOT}/src/in_memory_document.cpp")
target_include_directories(raster-source-fuzzer PRIVATE "${GRPARSE_ROOT}/include")
target_link_libraries(raster-source-fuzzer PRIVATE
  ${OpenCV_LIBS} Threads::Threads fuzz_flags)
