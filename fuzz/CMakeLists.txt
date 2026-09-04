# Standalone libFuzzer project for the two ingest doors (the code paths that
# touch attacker-controlled bytes before any model runs):
#   pdf-source-fuzzer    - Poppler-backed PdfPageSource: open + digital text
#   raster-source-fuzzer - OpenCV imdecode-backed RasterPageSource: open + decode
#
# Deliberately NOT part of the top-level build: the doors need only OpenCV,
# Poppler, and the headers under ../include, so this project skips the
# gRPC/ONNX Runtime fetch and configures in seconds.  See fuzz/README.md.
cmake_minimum_required(VERSION 3.20)
project(grparse-fuzz LANGUAGES CXX)

if(NOT CMAKE_CXX_COMPILER_ID MATCHES "Clang")
  message(FATAL_ERROR
    "libFuzzer needs Clang; configure with -DCMAKE_CXX_COMPILER=clang++")
endif()

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

find_package(OpenCV 4.5 REQUIRED COMPONENTS core imgcodecs imgproc)
find_package(PkgConfig REQUIRED)
find_package(Threads REQUIRED)
pkg_check_modules(POPPLER_CPP REQUIRED IMPORTED_TARGET poppler-cpp)

set(GRPARSE_ROOT "${CMAKE_CURRENT_SOURCE_DIR}/..")

add_library(fuzz_flags INTERFACE)
target_compile_options(fuzz_flags INTERFACE
  -g -O1 -fno-omit-frame-pointer -fsanitize=fuzzer,address,undefined)
target_link_options(fuzz_flags INTERFACE -fsanitize=fuzzer,address,undefined)
# Compile out the gRPC-backed PDF sources: a fuzzer must never dial a
# backend, and this project deliberately skips the gRPC fetch.
target_compile_definitions(fuzz_flags INTERFACE GRPARSE_FUZZ_STANDALONE)

foreach(door pdf-source raster-source)
  string(REPLACE "-" "_" door_source "${door}")
  add_executable(${door}-fuzzer
    "${door_source}_fuzzer.cpp"
    "${GRPARSE_ROOT}/src/in_memory_document.cpp")
  target_include_directories(${door}-fuzzer PRIVATE "${GRPARSE_ROOT}/include")
  target_link_libraries(${door}-fuzzer PRIVATE
    ${OpenCV_LIBS} PkgConfig::POPPLER_CPP Threads::Threads fuzz_flags)
endforeach()
