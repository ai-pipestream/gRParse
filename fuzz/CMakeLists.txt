# Standalone libFuzzer project for the in-process doors: the code paths that
# read attacker-controlled request bytes before any model or collector runs.
#   raster-source-fuzzer       - OpenCV imdecode-backed RasterPageSource
#   cfb-fuzzer                 - the [MS-CFB] compound-file walker
#   content-sniff-fuzzer       - resolve_mimetype, the byte sniff, input_format_for
#   confluence-storage-fuzzer  - the wiki storage XHTML parser and its fold
#   base64-differential-fuzzer - simdutf base64 against the scalar oracle
# PDFs never parse in this process; they go to a PdfBackendService, which
# fuzzes its own engine.
#
# Deliberately NOT part of the top-level build: the doors need only OpenCV,
# the system protobuf (for the Document and InputFormat messages), simdutf
# (fetched at the tag the top-level build pins) and the sources they test,
# so this project skips the gRPC/ONNX Runtime fetch and configures in
# seconds. See fuzz/README.md.
cmake_minimum_required(VERSION 3.20)
project(grparse-fuzz LANGUAGES CXX)

if(NOT CMAKE_CXX_COMPILER_ID MATCHES "Clang")
  message(FATAL_ERROR
    "libFuzzer needs Clang; configure with -DCMAKE_CXX_COMPILER=clang++")
endif()

# The code base is C++23; the doors are compiled as they ship.
set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

find_package(OpenCV 4.5 REQUIRED COMPONENTS core imgcodecs imgproc)
find_package(Protobuf REQUIRED)
find_package(Threads REQUIRED)

set(GRPARSE_ROOT "${CMAKE_CURRENT_SOURCE_DIR}/..")

add_library(fuzz_flags INTERFACE)
target_compile_options(fuzz_flags INTERFACE
  -g -O1 -fno-omit-frame-pointer -fsanitize=fuzzer,address,undefined)
target_link_options(fuzz_flags INTERFACE -fsanitize=fuzzer,address,undefined)
# Compile out the gRPC-backed PDF dispatch: a fuzzer must never dial a
# backend, and this project deliberately skips the gRPC fetch.
target_compile_definitions(fuzz_flags INTERFACE GRPARSE_FUZZ_STANDALONE)
target_include_directories(fuzz_flags INTERFACE
  "${GRPARSE_ROOT}/include" "${GRPARSE_ROOT}/src" "${CMAKE_CURRENT_SOURCE_DIR}")

# ---- raster ingest door ------------------------------------------------------
add_executable(raster-source-fuzzer
  raster_source_fuzzer.cpp
  "${GRPARSE_ROOT}/src/in_memory_document.cpp")
target_link_libraries(raster-source-fuzzer PRIVATE
  ${OpenCV_LIBS} Threads::Threads fuzz_flags)

# ---- the messages the doors build ------------------------------------------
# The flat protos staged under their import paths, as the top-level build
# stages them, and compiled with the system protoc: only the closure of
# document.proto (the confluence fold), office_service.proto (the integrity
# walk's header) and parse_types.proto (input_format_for's enum). The one
# change on the way: the debug_redact field option, which protoc before 22
# (Ubuntu 24.04 ships 3.21) does not know, is dropped. It only hides
# credential fields from DebugString, which no fuzz target prints.
set(FUZZ_PROTO_DIR "${CMAKE_CURRENT_BINARY_DIR}/proto")
set(FUZZ_PROTOS
  "document.proto=ai/pipestream/document/v1/document.proto"
  "office_service.proto=ai/pipestream/office/v1/office_service.proto"
  "parse_types.proto=ai/pipestream/parse/v1/parse_types.proto"
  "collectors/opennlp_document.proto=org/apache/opennlp/grpc/v1/opennlp_document.proto"
  "collectors/opennlp_annotations.proto=org/apache/opennlp/grpc/v1/opennlp_annotations.proto"
  "collectors/asr_service.proto=ai/pipestream/asr/v1/asr_service.proto"
  "collectors/ebcdic.proto=ai/pipestream/ebcdic/v1/ebcdic.proto"
  "collectors/lolhtml_types.proto=lolhtml/v1/types.proto"
  "collectors/lolhtml_service.proto=lolhtml/v1/lolhtml_service.proto")
set(FUZZ_PROTO_FILES)
set(FUZZ_PROTO_SOURCES)
foreach(mapping IN LISTS FUZZ_PROTOS)
  string(REPLACE "=" ";" pair "${mapping}")
  list(GET pair 0 source)
  list(GET pair 1 staged)
  configure_file("${GRPARSE_ROOT}/${source}" "${FUZZ_PROTO_DIR}/${staged}.in" COPYONLY)
  file(READ "${FUZZ_PROTO_DIR}/${staged}.in" proto_text)
  string(REPLACE " [debug_redact = true]" "" proto_text "${proto_text}")
  set(staged_text "")
  if(EXISTS "${FUZZ_PROTO_DIR}/${staged}")
    file(READ "${FUZZ_PROTO_DIR}/${staged}" staged_text)
  endif()
  if(NOT staged_text STREQUAL proto_text)  # rewritten only on a change
    file(WRITE "${FUZZ_PROTO_DIR}/${staged}" "${proto_text}")
  endif()
  list(APPEND FUZZ_PROTO_FILES "${FUZZ_PROTO_DIR}/${staged}")
  string(REGEX REPLACE "\\.proto$" "" stem "${staged}")
  list(APPEND FUZZ_PROTO_SOURCES "${FUZZ_PROTO_DIR}/${stem}.pb.cc")
endforeach()
set(FUZZ_PROTO_HEADERS ${FUZZ_PROTO_SOURCES})
list(TRANSFORM FUZZ_PROTO_HEADERS REPLACE "\\.pb\\.cc$" ".pb.h")
add_custom_command(
  OUTPUT ${FUZZ_PROTO_SOURCES} ${FUZZ_PROTO_HEADERS}
  COMMAND protobuf::protoc --proto_path=${FUZZ_PROTO_DIR} --cpp_out=${FUZZ_PROTO_DIR}
          ${FUZZ_PROTO_FILES}
  DEPENDS ${FUZZ_PROTO_FILES}
  COMMENT "Generating the Document and parse-type messages")
# Generated code is not under test: it compiles without the sanitizers and
# the coverage instrumentation, which keeps its coverage out of the fuzzers'
# feedback and the clean build of this project under two minutes.
add_library(fuzz_protos OBJECT ${FUZZ_PROTO_SOURCES})
target_compile_options(fuzz_protos PRIVATE -O1 -g0)
target_include_directories(fuzz_protos PUBLIC "${FUZZ_PROTO_DIR}")
target_link_libraries(fuzz_protos PUBLIC protobuf::libprotobuf)

# ---- content sniff and the compound-file walker ----------------------------
add_library(fuzz_content_sniff OBJECT
  "${GRPARSE_ROOT}/src/content_sniff.cpp"
  "${GRPARSE_ROOT}/src/confluence_storage_format.cpp"
  "${GRPARSE_ROOT}/src/confluence/storage_text.cpp")
target_link_libraries(fuzz_content_sniff PUBLIC fuzz_flags)

add_executable(cfb-fuzzer cfb_fuzzer.cpp)
target_link_libraries(cfb-fuzzer PRIVATE fuzz_content_sniff fuzz_flags)

add_executable(content-sniff-fuzzer
  content_sniff_fuzzer.cpp
  "${GRPARSE_ROOT}/src/input_format.cpp")
target_link_libraries(content-sniff-fuzzer PRIVATE fuzz_content_sniff fuzz_protos fuzz_flags)

# ---- wiki storage parser and fold ------------------------------------------
add_executable(confluence-storage-fuzzer
  confluence_storage_fuzzer.cpp
  "${GRPARSE_ROOT}/src/confluence/storage_blocks.cpp"
  "${GRPARSE_ROOT}/src/confluence/storage_fold.cpp"
  "${GRPARSE_ROOT}/src/confluence/storage_node.cpp"
  "${GRPARSE_ROOT}/src/confluence/storage_parser.cpp"
  "${GRPARSE_ROOT}/src/confluence/storage_table.cpp"
  "${GRPARSE_ROOT}/src/confluence/storage_text.cpp"
  "${GRPARSE_ROOT}/src/office_fold/integrity_check.cpp")
target_link_libraries(confluence-storage-fuzzer PRIVATE fuzz_protos fuzz_flags)

# ---- base64 differential ---------------------------------------------------
# simdutf at the tag the top-level CMakeLists.txt pins, instrumented like the
# code under test so the fuzzer sees coverage inside the vector kernels.
include(FetchContent)
set(SIMDUTF_TESTS OFF CACHE BOOL "" FORCE)
set(SIMDUTF_TOOLS OFF CACHE BOOL "" FORCE)
set(SIMDUTF_BENCHMARKS OFF CACHE BOOL "" FORCE)
set(SIMDUTF_INSTALL OFF CACHE BOOL "" FORCE)
FetchContent_Declare(simdutf
  GIT_REPOSITORY https://github.com/simdutf/simdutf.git
  GIT_TAG v9.2.0
  GIT_SHALLOW TRUE)
FetchContent_MakeAvailable(simdutf)
if(TARGET simdutf-nostdlibcxx)  # a variant nothing here links
  set_target_properties(simdutf-nostdlibcxx PROPERTIES EXCLUDE_FROM_ALL ON)
endif()
target_compile_options(simdutf PRIVATE
  -g -O1 -fno-omit-frame-pointer -fsanitize=fuzzer-no-link,address,undefined)

add_executable(base64-differential-fuzzer
  base64_differential_fuzzer.cpp
  "${GRPARSE_ROOT}/src/base64.cpp")
target_link_libraries(base64-differential-fuzzer PRIVATE simdutf::simdutf fuzz_flags)
