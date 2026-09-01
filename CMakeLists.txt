cmake_minimum_required(VERSION 3.20)
project(gRParse VERSION 0.1.0 LANGUAGES CXX C)

include(CTest)

# C++23: the newest published standard with complete GCC 15 support.
# zxing-cpp 3.x requires at least C++20 in its public headers.
set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_POSITION_INDEPENDENT_CODE ON)
# Everything links statically into the two binaries; the only shared libraries
# the runtime image stages are ONNX Runtime's (imported below).  Pinned OFF
# because gRPC 1.83.0's CMake flips the default when it is left unset, which
# produced a green build whose server could not load its own libraries.
set(BUILD_SHARED_LIBS OFF)

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
  # Deliberately pinned behind upstream head: the commits since add only
  # DirectML/GPU/C-API features this build does not use, and the session-ep
  # patch below is written against these sources (it does not apply at
  # head). Re-pin only together with a patch rebase.
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
# Intel GPU/CPU/NPU plugins.  `cpu` is Microsoft's plain CPU release, the only
# package published for both x86_64 and aarch64, and what the multi-arch
# Dockerfile.cpu image links.
set(GRPARSE_ORT_PACKAGE "cuda" CACHE STRING
  "ONNX Runtime package to link: cuda, openvino, or cpu")
if(GRPARSE_ORT_PACKAGE STREQUAL "cuda")
  FetchContent_Declare(onnxruntime
    URL https://github.com/microsoft/onnxruntime/releases/download/v1.29.0/onnxruntime-linux-x64-gpu_cuda13-1.29.0.tgz
    URL_HASH SHA256=844c64acfc43ab9423215c26493055ea229268e28283146cc644ecef0bdae048
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
elseif(GRPARSE_ORT_PACKAGE STREQUAL "cpu")
  if(CMAKE_SYSTEM_PROCESSOR MATCHES "^(aarch64|arm64)$")
    set(GRPARSE_ORT_CPU_ARCH "aarch64")
    set(GRPARSE_ORT_CPU_SHA256
      "e1799098ebc054b370f6176a450f158720f297818c613e5dc99b92e2ec82346f")
  elseif(CMAKE_SYSTEM_PROCESSOR MATCHES "^(x86_64|AMD64)$")
    set(GRPARSE_ORT_CPU_ARCH "x64")
    set(GRPARSE_ORT_CPU_SHA256
      "c3fddc4f139a045b0c4902c57410f0694f1c2fdf9b6939fbe38b1aeae7cd14ba")
  else()
    message(FATAL_ERROR "GRPARSE_ORT_PACKAGE=cpu: no ONNX Runtime CPU release "
      "for ${CMAKE_SYSTEM_PROCESSOR} (x86_64 and aarch64 only)")
  endif()
  FetchContent_Declare(onnxruntime
    URL https://github.com/microsoft/onnxruntime/releases/download/v1.29.0/onnxruntime-linux-${GRPARSE_ORT_CPU_ARCH}-1.29.0.tgz
    URL_HASH SHA256=${GRPARSE_ORT_CPU_SHA256}
    DOWNLOAD_EXTRACT_TIMESTAMP TRUE)
  FetchContent_MakeAvailable(onnxruntime)
  set(GRPARSE_ORT_LIBRARY "${onnxruntime_SOURCE_DIR}/lib/libonnxruntime.so")
else()
  message(FATAL_ERROR "GRPARSE_ORT_PACKAGE must be cuda, openvino, or cpu")
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
  # 3.x needs C++20 in its public headers, which the project now builds as.
  GIT_TAG v3.1.1
  GIT_SHALLOW TRUE)
FetchContent_MakeAvailable(zxing)

# yaml-cpp backs the YAML document export: the document's canonical JSON
# re-emitted in block style. Nothing parses YAML at runtime, and none of the
# optional tooling is built.
set(YAML_CPP_BUILD_CONTRIB OFF CACHE BOOL "" FORCE)
set(YAML_CPP_BUILD_TOOLS OFF CACHE BOOL "" FORCE)
set(YAML_CPP_BUILD_TESTS OFF CACHE BOOL "" FORCE)
set(YAML_CPP_INSTALL OFF CACHE BOOL "" FORCE)
FetchContent_Declare(yaml-cpp
  GIT_REPOSITORY https://github.com/jbeder/yaml-cpp.git
  GIT_TAG yaml-cpp-0.9.0
  GIT_SHALLOW TRUE)
# 0.9.0 needs neither the 0.8.0-era <cstdint> backport patch (fixed
# upstream) nor the CMake 4 policy floor (it declares 3.5...3.30).
FetchContent_MakeAvailable(yaml-cpp)

# simdutf provides runtime-dispatched vector kernels (AVX-512/AVX2/SSE on
# x86-64, NEON on aarch64) for the base64 hot paths: every uploaded document
# decodes and every page image and picture crop encodes through them. The
# scalar implementations stay as the accept/reject contract oracle.
set(SIMDUTF_TESTS OFF CACHE BOOL "" FORCE)
set(SIMDUTF_TOOLS OFF CACHE BOOL "" FORCE)
set(SIMDUTF_BENCHMARKS OFF CACHE BOOL "" FORCE)
set(SIMDUTF_INSTALL OFF CACHE BOOL "" FORCE)
FetchContent_Declare(simdutf
  GIT_REPOSITORY https://github.com/simdutf/simdutf.git
  GIT_TAG v9.1.0
  GIT_SHALLOW TRUE)
FetchContent_MakeAvailable(simdutf)

# miniz supplies the raw deflate compressor and the CRC-32 the ZIP target
# writes its archives with; the ZIP container itself is written here, because
# no stock archiver produces the byte-identical output a reproducible bundle
# needs.  The release artifact is the amalgamated distribution (two files, no
# build system of its own), so SOURCE_SUBDIR names a directory that does not
# exist: FetchContent then populates without trying to configure it.
FetchContent_Declare(miniz SOURCE_SUBDIR disabled
  URL https://github.com/richgel999/miniz/releases/download/3.1.2/miniz-3.1.2.zip
  URL_HASH SHA256=f0446d863f9c19926ad9483c523fdc42e42b8d4a6a431d27e09d49c79a140d9a)
FetchContent_MakeAvailable(miniz)
add_library(miniz STATIC "${miniz_SOURCE_DIR}/miniz.c")
target_include_directories(miniz PUBLIC "${miniz_SOURCE_DIR}")

find_package(OpenCV 4.5 REQUIRED COMPONENTS core imgcodecs imgproc)
find_package(PkgConfig REQUIRED)
find_package(Threads REQUIRED)
# libcurl carries the S3 target's signed PUTs.  The distribution's own, not a
# vendored copy: the three images already ship a TLS stack, and the signing is
# this repo's (Signature V4 over the request headers), so nothing here needs
# an AWS SDK.
find_package(CURL REQUIRED)
pkg_check_modules(POPPLER_CPP REQUIRED IMPORTED_TARGET poppler-cpp)

# The server links gRPC's vendored BoringSSL statically and libcurl
# dynamically, and libcurl's TLS backend is OpenSSL.  The two define the same
# symbol names, so an executable that exported the static definitions would
# hand libcurl BoringSSL's SSL_* at load time and break it on the first https
# upload.  Keeping every static archive's symbols out of the dynamic symbol
# table is what holds the two apart.
add_library(grparse_curl INTERFACE)
target_link_libraries(grparse_curl INTERFACE CURL::libcurl)
target_link_options(grparse_curl INTERFACE "LINKER:--exclude-libs,ALL")

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
  GIT_TAG v1.83.0
  GIT_SHALLOW TRUE
  GIT_SUBMODULES_RECURSE TRUE)
FetchContent_MakeAvailable(grpc)

set(GENERATED_DIR "${CMAKE_CURRENT_BINARY_DIR}/generated")
set(STAGED_PROTO_DIR "${CMAKE_CURRENT_BINARY_DIR}/proto")
file(MAKE_DIRECTORY "${GENERATED_DIR}")
file(MAKE_DIRECTORY "${STAGED_PROTO_DIR}/ai/pipestream/document/v1")
file(MAKE_DIRECTORY "${STAGED_PROTO_DIR}/ai/pipestream/parse/v1")
file(MAKE_DIRECTORY "${STAGED_PROTO_DIR}/ai/pipestream/office/v1")
file(MAKE_DIRECTORY "${STAGED_PROTO_DIR}/ai/pipestream/asr/v1")
file(MAKE_DIRECTORY "${STAGED_PROTO_DIR}/ai/pipestream/email/v1")
file(MAKE_DIRECTORY "${STAGED_PROTO_DIR}/ai/pipestream/xml/v1")
file(MAKE_DIRECTORY "${STAGED_PROTO_DIR}/ai/pipestream/ebcdic/v1")
file(MAKE_DIRECTORY "${STAGED_PROTO_DIR}/ai/pipestream/epub/v1")
file(MAKE_DIRECTORY "${STAGED_PROTO_DIR}/ai/pipestream/markup/v1")
file(MAKE_DIRECTORY "${STAGED_PROTO_DIR}/lolhtml/v1")
file(MAKE_DIRECTORY "${STAGED_PROTO_DIR}/fastwarc/v1")
file(MAKE_DIRECTORY "${STAGED_PROTO_DIR}/ai/pipestream/pdf/v1")
configure_file(document.proto
  "${STAGED_PROTO_DIR}/ai/pipestream/document/v1/document.proto" COPYONLY)
configure_file(parse_types.proto
  "${STAGED_PROTO_DIR}/ai/pipestream/parse/v1/parse_types.proto" COPYONLY)
configure_file(parse.proto
  "${STAGED_PROTO_DIR}/ai/pipestream/parse/v1/parse.proto" COPYONLY)
configure_file(parse_stream.proto
  "${STAGED_PROTO_DIR}/ai/pipestream/parse/v1/parse_stream.proto" COPYONLY)
configure_file(office_service.proto
  "${STAGED_PROTO_DIR}/ai/pipestream/office/v1/office_service.proto" COPYONLY)
# The collector wire contracts, vendored byte-identical from their repos
# (collectors/README.md). epub_types.proto stages under its import path name.
configure_file(collectors/asr_service.proto
  "${STAGED_PROTO_DIR}/ai/pipestream/asr/v1/asr_service.proto" COPYONLY)
configure_file(collectors/email_service.proto
  "${STAGED_PROTO_DIR}/ai/pipestream/email/v1/email_service.proto" COPYONLY)
configure_file(collectors/xml.proto
  "${STAGED_PROTO_DIR}/ai/pipestream/xml/v1/xml.proto" COPYONLY)
configure_file(collectors/xml_service.proto
  "${STAGED_PROTO_DIR}/ai/pipestream/xml/v1/xml_service.proto" COPYONLY)
configure_file(collectors/ebcdic.proto
  "${STAGED_PROTO_DIR}/ai/pipestream/ebcdic/v1/ebcdic.proto" COPYONLY)
configure_file(collectors/ebcdic_service.proto
  "${STAGED_PROTO_DIR}/ai/pipestream/ebcdic/v1/ebcdic_service.proto" COPYONLY)
configure_file(collectors/epub_types.proto
  "${STAGED_PROTO_DIR}/ai/pipestream/epub/v1/types.proto" COPYONLY)
configure_file(collectors/epub_service.proto
  "${STAGED_PROTO_DIR}/ai/pipestream/epub/v1/epub_service.proto" COPYONLY)
configure_file(collectors/markup.proto
  "${STAGED_PROTO_DIR}/ai/pipestream/markup/v1/markup.proto" COPYONLY)
configure_file(collectors/markup_service.proto
  "${STAGED_PROTO_DIR}/ai/pipestream/markup/v1/markup_service.proto" COPYONLY)
configure_file(collectors/lolhtml_types.proto
  "${STAGED_PROTO_DIR}/lolhtml/v1/types.proto" COPYONLY)
configure_file(collectors/lolhtml_service.proto
  "${STAGED_PROTO_DIR}/lolhtml/v1/lolhtml_service.proto" COPYONLY)
configure_file(collectors/warc.proto
  "${STAGED_PROTO_DIR}/fastwarc/v1/warc.proto" COPYONLY)
configure_file(collectors/warc_service.proto
  "${STAGED_PROTO_DIR}/fastwarc/v1/warc_service.proto" COPYONLY)
# pdf_types.proto stages under its import path name, like epub_types.proto.
configure_file(collectors/pdf_types.proto
  "${STAGED_PROTO_DIR}/ai/pipestream/pdf/v1/types.proto" COPYONLY)
configure_file(collectors/pdf_service.proto
  "${STAGED_PROTO_DIR}/ai/pipestream/pdf/v1/pdf_service.proto" COPYONLY)

set(DOCUMENT_PROTO "${STAGED_PROTO_DIR}/ai/pipestream/document/v1/document.proto")
set(PARSE_TYPES_PROTO "${STAGED_PROTO_DIR}/ai/pipestream/parse/v1/parse_types.proto")
set(PARSE_SERVICE_PROTO "${STAGED_PROTO_DIR}/ai/pipestream/parse/v1/parse.proto")
set(PARSE_STREAM_PROTO "${STAGED_PROTO_DIR}/ai/pipestream/parse/v1/parse_stream.proto")
set(OFFICE_PROTO "${STAGED_PROTO_DIR}/ai/pipestream/office/v1/office_service.proto")
set(COLLECTOR_PROTOS
  "${STAGED_PROTO_DIR}/ai/pipestream/asr/v1/asr_service.proto"
  "${STAGED_PROTO_DIR}/ai/pipestream/email/v1/email_service.proto"
  "${STAGED_PROTO_DIR}/ai/pipestream/xml/v1/xml.proto"
  "${STAGED_PROTO_DIR}/ai/pipestream/xml/v1/xml_service.proto"
  "${STAGED_PROTO_DIR}/ai/pipestream/ebcdic/v1/ebcdic.proto"
  "${STAGED_PROTO_DIR}/ai/pipestream/ebcdic/v1/ebcdic_service.proto"
  "${STAGED_PROTO_DIR}/ai/pipestream/epub/v1/types.proto"
  "${STAGED_PROTO_DIR}/ai/pipestream/epub/v1/epub_service.proto"
  "${STAGED_PROTO_DIR}/ai/pipestream/markup/v1/markup.proto"
  "${STAGED_PROTO_DIR}/ai/pipestream/markup/v1/markup_service.proto"
  "${STAGED_PROTO_DIR}/lolhtml/v1/types.proto"
  "${STAGED_PROTO_DIR}/lolhtml/v1/lolhtml_service.proto"
  "${STAGED_PROTO_DIR}/fastwarc/v1/warc.proto"
  "${STAGED_PROTO_DIR}/fastwarc/v1/warc_service.proto"
  "${STAGED_PROTO_DIR}/ai/pipestream/pdf/v1/types.proto"
  "${STAGED_PROTO_DIR}/ai/pipestream/pdf/v1/pdf_service.proto")
set(COLLECTOR_GENERATED
  "${GENERATED_DIR}/ai/pipestream/asr/v1/asr_service.pb.cc"
  "${GENERATED_DIR}/ai/pipestream/asr/v1/asr_service.pb.h"
  "${GENERATED_DIR}/ai/pipestream/asr/v1/asr_service.grpc.pb.cc"
  "${GENERATED_DIR}/ai/pipestream/asr/v1/asr_service.grpc.pb.h"
  "${GENERATED_DIR}/ai/pipestream/email/v1/email_service.pb.cc"
  "${GENERATED_DIR}/ai/pipestream/email/v1/email_service.pb.h"
  "${GENERATED_DIR}/ai/pipestream/email/v1/email_service.grpc.pb.cc"
  "${GENERATED_DIR}/ai/pipestream/email/v1/email_service.grpc.pb.h"
  "${GENERATED_DIR}/ai/pipestream/xml/v1/xml.pb.cc"
  "${GENERATED_DIR}/ai/pipestream/xml/v1/xml.pb.h"
  "${GENERATED_DIR}/ai/pipestream/xml/v1/xml.grpc.pb.cc"
  "${GENERATED_DIR}/ai/pipestream/xml/v1/xml.grpc.pb.h"
  "${GENERATED_DIR}/ai/pipestream/xml/v1/xml_service.pb.cc"
  "${GENERATED_DIR}/ai/pipestream/xml/v1/xml_service.pb.h"
  "${GENERATED_DIR}/ai/pipestream/xml/v1/xml_service.grpc.pb.cc"
  "${GENERATED_DIR}/ai/pipestream/xml/v1/xml_service.grpc.pb.h"
  "${GENERATED_DIR}/ai/pipestream/ebcdic/v1/ebcdic.pb.cc"
  "${GENERATED_DIR}/ai/pipestream/ebcdic/v1/ebcdic.pb.h"
  "${GENERATED_DIR}/ai/pipestream/ebcdic/v1/ebcdic.grpc.pb.cc"
  "${GENERATED_DIR}/ai/pipestream/ebcdic/v1/ebcdic.grpc.pb.h"
  "${GENERATED_DIR}/ai/pipestream/ebcdic/v1/ebcdic_service.pb.cc"
  "${GENERATED_DIR}/ai/pipestream/ebcdic/v1/ebcdic_service.pb.h"
  "${GENERATED_DIR}/ai/pipestream/ebcdic/v1/ebcdic_service.grpc.pb.cc"
  "${GENERATED_DIR}/ai/pipestream/ebcdic/v1/ebcdic_service.grpc.pb.h"
  "${GENERATED_DIR}/ai/pipestream/epub/v1/types.pb.cc"
  "${GENERATED_DIR}/ai/pipestream/epub/v1/types.pb.h"
  "${GENERATED_DIR}/ai/pipestream/epub/v1/types.grpc.pb.cc"
  "${GENERATED_DIR}/ai/pipestream/epub/v1/types.grpc.pb.h"
  "${GENERATED_DIR}/ai/pipestream/epub/v1/epub_service.pb.cc"
  "${GENERATED_DIR}/ai/pipestream/epub/v1/epub_service.pb.h"
  "${GENERATED_DIR}/ai/pipestream/epub/v1/epub_service.grpc.pb.cc"
  "${GENERATED_DIR}/ai/pipestream/epub/v1/epub_service.grpc.pb.h"
  "${GENERATED_DIR}/ai/pipestream/markup/v1/markup.pb.cc"
  "${GENERATED_DIR}/ai/pipestream/markup/v1/markup.pb.h"
  "${GENERATED_DIR}/ai/pipestream/markup/v1/markup.grpc.pb.cc"
  "${GENERATED_DIR}/ai/pipestream/markup/v1/markup.grpc.pb.h"
  "${GENERATED_DIR}/ai/pipestream/markup/v1/markup_service.pb.cc"
  "${GENERATED_DIR}/ai/pipestream/markup/v1/markup_service.pb.h"
  "${GENERATED_DIR}/ai/pipestream/markup/v1/markup_service.grpc.pb.cc"
  "${GENERATED_DIR}/ai/pipestream/markup/v1/markup_service.grpc.pb.h"
  "${GENERATED_DIR}/lolhtml/v1/types.pb.cc"
  "${GENERATED_DIR}/lolhtml/v1/types.pb.h"
  "${GENERATED_DIR}/lolhtml/v1/types.grpc.pb.cc"
  "${GENERATED_DIR}/lolhtml/v1/types.grpc.pb.h"
  "${GENERATED_DIR}/lolhtml/v1/lolhtml_service.pb.cc"
  "${GENERATED_DIR}/lolhtml/v1/lolhtml_service.pb.h"
  "${GENERATED_DIR}/lolhtml/v1/lolhtml_service.grpc.pb.cc"
  "${GENERATED_DIR}/lolhtml/v1/lolhtml_service.grpc.pb.h"
  "${GENERATED_DIR}/fastwarc/v1/warc.pb.cc"
  "${GENERATED_DIR}/fastwarc/v1/warc.pb.h"
  "${GENERATED_DIR}/fastwarc/v1/warc.grpc.pb.cc"
  "${GENERATED_DIR}/fastwarc/v1/warc.grpc.pb.h"
  "${GENERATED_DIR}/fastwarc/v1/warc_service.pb.cc"
  "${GENERATED_DIR}/fastwarc/v1/warc_service.pb.h"
  "${GENERATED_DIR}/fastwarc/v1/warc_service.grpc.pb.cc"
  "${GENERATED_DIR}/fastwarc/v1/warc_service.grpc.pb.h"
  "${GENERATED_DIR}/ai/pipestream/pdf/v1/types.pb.cc"
  "${GENERATED_DIR}/ai/pipestream/pdf/v1/types.pb.h"
  "${GENERATED_DIR}/ai/pipestream/pdf/v1/types.grpc.pb.cc"
  "${GENERATED_DIR}/ai/pipestream/pdf/v1/types.grpc.pb.h"
  "${GENERATED_DIR}/ai/pipestream/pdf/v1/pdf_service.pb.cc"
  "${GENERATED_DIR}/ai/pipestream/pdf/v1/pdf_service.pb.h"
  "${GENERATED_DIR}/ai/pipestream/pdf/v1/pdf_service.grpc.pb.cc"
  "${GENERATED_DIR}/ai/pipestream/pdf/v1/pdf_service.grpc.pb.h")
add_custom_command(
  OUTPUT "${GENERATED_DIR}/ai/pipestream/document/v1/document.pb.cc"
         "${GENERATED_DIR}/ai/pipestream/document/v1/document.pb.h"
         "${GENERATED_DIR}/ai/pipestream/parse/v1/parse_types.pb.cc"
         "${GENERATED_DIR}/ai/pipestream/parse/v1/parse_types.pb.h"
         "${GENERATED_DIR}/ai/pipestream/parse/v1/parse.pb.cc"
         "${GENERATED_DIR}/ai/pipestream/parse/v1/parse.pb.h"
         "${GENERATED_DIR}/ai/pipestream/parse/v1/parse.grpc.pb.cc"
         "${GENERATED_DIR}/ai/pipestream/parse/v1/parse.grpc.pb.h"
         "${GENERATED_DIR}/ai/pipestream/parse/v1/parse_stream.pb.cc"
         "${GENERATED_DIR}/ai/pipestream/parse/v1/parse_stream.pb.h"
         "${GENERATED_DIR}/ai/pipestream/parse/v1/parse_stream.grpc.pb.cc"
         "${GENERATED_DIR}/ai/pipestream/parse/v1/parse_stream.grpc.pb.h"
         "${GENERATED_DIR}/ai/pipestream/office/v1/office_service.pb.cc"
         "${GENERATED_DIR}/ai/pipestream/office/v1/office_service.pb.h"
         "${GENERATED_DIR}/ai/pipestream/office/v1/office_service.grpc.pb.cc"
         "${GENERATED_DIR}/ai/pipestream/office/v1/office_service.grpc.pb.h"
         ${COLLECTOR_GENERATED}
  COMMAND $<TARGET_FILE:protoc>
  ARGS --proto_path=${STAGED_PROTO_DIR}
       --proto_path=${protobuf_SOURCE_DIR}/src
       --cpp_out=${GENERATED_DIR}
       --grpc_out=${GENERATED_DIR}
       --plugin=protoc-gen-grpc=$<TARGET_FILE:grpc_cpp_plugin>
       ${DOCUMENT_PROTO} ${PARSE_TYPES_PROTO} ${PARSE_SERVICE_PROTO} ${PARSE_STREAM_PROTO}
       ${OFFICE_PROTO} ${COLLECTOR_PROTOS}
  DEPENDS document.proto parse_types.proto parse.proto parse_stream.proto office_service.proto
          collectors/asr_service.proto collectors/email_service.proto
          collectors/xml.proto collectors/xml_service.proto
          collectors/ebcdic.proto collectors/ebcdic_service.proto
          collectors/epub_types.proto collectors/epub_service.proto
          collectors/markup.proto collectors/markup_service.proto
          collectors/lolhtml_types.proto collectors/lolhtml_service.proto
          collectors/warc.proto collectors/warc_service.proto
          collectors/pdf_types.proto collectors/pdf_service.proto
          protoc grpc_cpp_plugin
  VERBATIM)
# STATIC explicitly, same reason as grparse_core below.
add_library(grparse_proto STATIC
  "${GENERATED_DIR}/ai/pipestream/document/v1/document.pb.cc"
  "${GENERATED_DIR}/ai/pipestream/parse/v1/parse_types.pb.cc"
  "${GENERATED_DIR}/ai/pipestream/parse/v1/parse.pb.cc"
  "${GENERATED_DIR}/ai/pipestream/parse/v1/parse.grpc.pb.cc"
  "${GENERATED_DIR}/ai/pipestream/parse/v1/parse_stream.pb.cc"
  "${GENERATED_DIR}/ai/pipestream/parse/v1/parse_stream.grpc.pb.cc"
  "${GENERATED_DIR}/ai/pipestream/office/v1/office_service.pb.cc"
  "${GENERATED_DIR}/ai/pipestream/office/v1/office_service.grpc.pb.cc"
  "${GENERATED_DIR}/ai/pipestream/asr/v1/asr_service.pb.cc"
  "${GENERATED_DIR}/ai/pipestream/asr/v1/asr_service.grpc.pb.cc"
  "${GENERATED_DIR}/ai/pipestream/email/v1/email_service.pb.cc"
  "${GENERATED_DIR}/ai/pipestream/email/v1/email_service.grpc.pb.cc"
  "${GENERATED_DIR}/ai/pipestream/xml/v1/xml.pb.cc"
  "${GENERATED_DIR}/ai/pipestream/xml/v1/xml.grpc.pb.cc"
  "${GENERATED_DIR}/ai/pipestream/xml/v1/xml_service.pb.cc"
  "${GENERATED_DIR}/ai/pipestream/xml/v1/xml_service.grpc.pb.cc"
  "${GENERATED_DIR}/ai/pipestream/ebcdic/v1/ebcdic.pb.cc"
  "${GENERATED_DIR}/ai/pipestream/ebcdic/v1/ebcdic.grpc.pb.cc"
  "${GENERATED_DIR}/ai/pipestream/ebcdic/v1/ebcdic_service.pb.cc"
  "${GENERATED_DIR}/ai/pipestream/ebcdic/v1/ebcdic_service.grpc.pb.cc"
  "${GENERATED_DIR}/ai/pipestream/epub/v1/types.pb.cc"
  "${GENERATED_DIR}/ai/pipestream/epub/v1/types.grpc.pb.cc"
  "${GENERATED_DIR}/ai/pipestream/epub/v1/epub_service.pb.cc"
  "${GENERATED_DIR}/ai/pipestream/epub/v1/epub_service.grpc.pb.cc"
  "${GENERATED_DIR}/ai/pipestream/markup/v1/markup.pb.cc"
  "${GENERATED_DIR}/ai/pipestream/markup/v1/markup.grpc.pb.cc"
  "${GENERATED_DIR}/ai/pipestream/markup/v1/markup_service.pb.cc"
  "${GENERATED_DIR}/ai/pipestream/markup/v1/markup_service.grpc.pb.cc"
  "${GENERATED_DIR}/lolhtml/v1/types.pb.cc"
  "${GENERATED_DIR}/lolhtml/v1/types.grpc.pb.cc"
  "${GENERATED_DIR}/lolhtml/v1/lolhtml_service.pb.cc"
  "${GENERATED_DIR}/lolhtml/v1/lolhtml_service.grpc.pb.cc"
  "${GENERATED_DIR}/fastwarc/v1/warc.pb.cc"
  "${GENERATED_DIR}/fastwarc/v1/warc.grpc.pb.cc"
  "${GENERATED_DIR}/fastwarc/v1/warc_service.pb.cc"
  "${GENERATED_DIR}/fastwarc/v1/warc_service.grpc.pb.cc"
  "${GENERATED_DIR}/ai/pipestream/pdf/v1/types.pb.cc"
  "${GENERATED_DIR}/ai/pipestream/pdf/v1/types.grpc.pb.cc"
  "${GENERATED_DIR}/ai/pipestream/pdf/v1/pdf_service.pb.cc"
  "${GENERATED_DIR}/ai/pipestream/pdf/v1/pdf_service.grpc.pb.cc")
target_include_directories(grparse_proto PUBLIC "${GENERATED_DIR}")
target_link_libraries(grparse_proto PUBLIC grpc++)

# STATIC explicitly: a dependency's CMake can flip the ambient
# BUILD_SHARED_LIBS default, and a shared grparse_core would be invisible to
# the Dockerfile's runtime-closure staging (its ldd pass runs without the
# build tree mounted).  gRPC 1.83.0 did exactly that: green build, image
# whose server could not load libgrparse_core.so.
add_library(grparse_core STATIC
  src/barcode_decoder.cpp
  src/base64.cpp
  src/call_executor.cpp
  src/collector_coordinator.cpp
  src/confluence_storage.cpp
  src/docling_map.cpp
  src/document_merge.cpp
  src/document_repair.cpp
  src/epub_book.cpp
  src/document_render.cpp
  src/render/renderer_base.cpp
  src/render/load_normalization.cpp
  src/render/display_width.cpp
  src/render/canonical_json_writer.cpp
  src/render/canonical_json_renderer.cpp
  src/render/markdown_renderer.cpp
  src/render/html_renderer.cpp
  src/render/gdocs_renderer.cpp
  src/render/doctags_renderer.cpp
  src/render/doclang_renderer.cpp
  src/render/vtt_renderer.cpp
  src/chunking/token_counter.cpp
  src/chunking/sentence_rules.cpp
  src/chunking/chunker.cpp
  src/document_collectors.cpp
  src/document_parser_service.cpp
  src/office_collector.cpp
  src/office_cv_enrichment.cpp
  src/figure_classifier.cpp
  src/document_assembly.cpp
  src/in_memory_document.cpp
  src/ocr_engine.cpp
  src/layout_decode.cpp
  src/layout_engine.cpp
  src/page_previews.cpp
  src/page_projection.cpp
  src/page_scheduler.cpp
  src/prometheus_metrics.cpp
  src/reading_order.cpp
  src/region_geometry.cpp
  src/table_structure.cpp
  src/table_structure_engine.cpp
  src/targets/bundle.cpp
  src/targets/s3_client.cpp
  src/targets/s3_uploader.cpp
  src/targets/sha256.cpp
  src/targets/sigv4.cpp
  src/targets/target_step.cpp
  src/targets/zip_writer.cpp
  src/text_geometry.cpp)
target_include_directories(grparse_core PUBLIC include)
# The served version string names the accelerator flavor this build carries.
target_compile_definitions(grparse_core
  PRIVATE GRPARSE_ORT_PACKAGE_NAME="${GRPARSE_ORT_PACKAGE}")
target_link_libraries(grparse_core
  # grparse_curl is PUBLIC for its link options alone: they belong to every
  # executable that ends up with both TLS stacks in its link closure.
  PUBLIC grparse_proto rapidocr PkgConfig::POPPLER_CPP Threads::Threads grparse_curl
  PRIVATE ZXing::ZXing yaml-cpp::yaml-cpp simdutf::simdutf miniz grparse_flags)

add_executable(grparse-server src/main.cpp)
target_link_libraries(grparse-server PRIVATE grparse_core grparse_flags grpc++_reflection)

add_executable(grparse-stream-client src/stream_pdf_client.cpp)
target_link_libraries(grparse-stream-client PRIVATE grparse_proto grparse_flags)

if(BUILD_TESTING)
  set(GRPARSE_TESTS
    barcode-decoder-test
    base64-test
    call-executor-test
    canonical-json-test
    chunking-test
    collector-coordinator-test
    confluence-storage-test
    docling-map-test
    document-assembly-test
    document-collectors-test
    document-merge-test
    document-render-test
    document-repair-test
    epub-book-test
    figure-classifier-test
    gdocs-renderer-test
    layout-decode-test
    layout-engine-test
    office-collector-test
    office-cv-enrichment-test
    ort-session-ep-test
    page-previews-test
    page-projection-test
    page-scheduler-test
    pdf-source-test
    prometheus-metrics-test
    pdf-source-concurrency-test
    raster-source-test
    reading-order-test
    region-geometry-test
    resource-pool-test
    streaming-service-test
    table-structure-engine-test
    table-structure-test
    targets-test
    text-geometry-test
    warc-stub-wire-test)
  foreach(test_name IN LISTS GRPARSE_TESTS)
    string(REPLACE "-" "_" test_source "${test_name}")
    add_executable(${test_name} "tests/${test_source}.cpp")
    target_link_libraries(${test_name} PRIVATE grparse_core grparse_flags)
    add_test(NAME ${test_name} COMMAND ${test_name})
  endforeach()
  # Opt-in oracle battery (eval/README.md): skips (77) unless GRPARSE_TARGET,
  # VLM_ENDPOINT and EVAL_CORPUS are set in the environment, so a green
  # build never claims it ran.
  find_package(Python3 COMPONENTS Interpreter)
  if(Python3_Interpreter_FOUND)
    add_test(NAME vlm-oracle-eval
      COMMAND ${Python3_EXECUTABLE} ${CMAKE_SOURCE_DIR}/eval/compare_vlm.py)
    set_tests_properties(vlm-oracle-eval PROPERTIES SKIP_RETURN_CODE 77 LABELS eval)
    # Structural scorecard (eval/README.md): scores a running gRParse at
    # GRPARSE_TARGET against eval/scorecard/baseline; skips (77) when the
    # service is unreachable or grpcio is absent, fails (1) on a regression.
    add_test(NAME structural-scorecard
      COMMAND ${Python3_EXECUTABLE} ${CMAKE_SOURCE_DIR}/eval/scorecard/run.py
      WORKING_DIRECTORY ${CMAKE_SOURCE_DIR})
    set_tests_properties(structural-scorecard PROPERTIES SKIP_RETURN_CODE 77 LABELS eval)
  endif()
  # Development utility for the canonical JSON validation harness
  # (scripts/validate_canonical_json.py): Document in, canonical JSON out.
  # Built with the tests, never installed or staged into images.
  add_executable(grparse-canonical-json-tool tests/canonical_json_tool.cpp)
  target_link_libraries(grparse-canonical-json-tool PRIVATE grparse_core grparse_flags)
  # Development utility for the Markdown validation harness
  # (scripts/validate_markdown.py): Document in, Markdown out.  Built with the
  # tests, never installed or staged into images.
  add_executable(grparse-markdown-tool tests/markdown_tool.cpp)
  target_link_libraries(grparse-markdown-tool PRIVATE grparse_core grparse_flags)
  # Development utility for layout measurement: a document in, per-page
  # detections, label counts, and device timings out on whichever execution
  # provider the environment selects.  Built with the tests, never installed
  # or staged into images.
  add_executable(grparse-layout-tool tests/layout_tool.cpp)
  target_include_directories(grparse-layout-tool PRIVATE compat)
  target_link_libraries(grparse-layout-tool PRIVATE grparse_core grparse_ort_ep grparse_flags)
  # The umbrella target is what image builds compile.  Building it (instead of
  # naming test binaries in each Dockerfile) means a test added here can never
  # be silently absent from an image's ctest run ("Not Run" failures).
  add_custom_target(grparse-tests DEPENDS ${GRPARSE_TESTS})
  # The scheduler and streaming tests exercise multi-worker paths; keep their
  # ceiling well above the ~1s they need so a stall fails instead of hanging CI.
  # The label keeps `ctest -L grparse` clear of tests registered by fetched
  # dependencies (re2 registers ~20 of its own).  The ceiling is a cache
  # variable because emulated multi-arch image builds (arm64 under QEMU) run
  # the same suite one to two orders of magnitude slower than native.
  set(GRPARSE_TEST_TIMEOUT 120 CACHE STRING
    "Per-test ctest timeout in seconds for the grparse suite")
  set_tests_properties(${GRPARSE_TESTS} PROPERTIES
    TIMEOUT ${GRPARSE_TEST_TIMEOUT} LABELS grparse)
  # The layout golden needs the real model file; without it the test skips
  # (exit 77) so environments that never download models stay green while any
  # environment with models proves the decode against the reference output.
  set_tests_properties(figure-classifier-test layout-engine-test table-structure-engine-test PROPERTIES
    SKIP_RETURN_CODE 77
    ENVIRONMENT
      "GRPARSE_TEST_MODELS_DIR=${CMAKE_CURRENT_SOURCE_DIR}/models;GRPARSE_TEST_DATA_DIR=${CMAKE_CURRENT_SOURCE_DIR}/tests/data")
  # ZXing is compiled in, so the barcode tests never skip; they only need the
  # committed QR fixture.  The scheduler test loads the same fixture to prove
  # the class-triggered decode end to end, and the office enrichment test
  # runs it through a fake detector to prove the hybrid leg without models.
  set_tests_properties(barcode-decoder-test page-scheduler-test office-cv-enrichment-test PROPERTIES
    ENVIRONMENT "GRPARSE_TEST_DATA_DIR=${CMAKE_CURRENT_SOURCE_DIR}/tests/data")
endif()

install(TARGETS grparse-server grparse-stream-client RUNTIME DESTINATION bin)

# ---- data plane: content sniffing and data-change counters ------------------
# Appended as one block: the sources join grparse_core and the tests join the
# grparse suite with the same timeout and label as the list above. The
# sniffer test reads the scorecard corpus when it is present, so a signature
# the synthetic cases model wrongly fails against the real fixtures.
target_sources(grparse_core PRIVATE
  src/content_sniff.cpp
  src/data_totals.cpp)
if(BUILD_TESTING)
  set(GRPARSE_DATA_TESTS content-sniff-test)
  foreach(test_name IN LISTS GRPARSE_DATA_TESTS)
    string(REPLACE "-" "_" test_source "${test_name}")
    add_executable(${test_name} "tests/${test_source}.cpp")
    target_link_libraries(${test_name} PRIVATE grparse_core grparse_flags)
    add_test(NAME ${test_name} COMMAND ${test_name})
  endforeach()
  add_dependencies(grparse-tests ${GRPARSE_DATA_TESTS})
  set_tests_properties(${GRPARSE_DATA_TESTS} PROPERTIES
    TIMEOUT ${GRPARSE_TEST_TIMEOUT} LABELS grparse)
  set_tests_properties(content-sniff-test PROPERTIES
    ENVIRONMENT "GRPARSE_TEST_CORPUS_DIR=${CMAKE_CURRENT_SOURCE_DIR}/tests/golden/corpus")
endif()
