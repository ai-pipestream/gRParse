# Native HailoRT for the Hailo-10H (src/hailo/) behind grparse::InferenceModel
# (compat/grparse_inference_model.h). ON in the arm64 image built with
# GRPARSE_HAILORT_VERSION (Dockerfile.cpu) only. The tiling, windowing and
# placement policy (src/hailo/hailo_ocr*.cpp, hailo_placement.cpp) builds in
# every flavor and is tested in every flavor; only hailo_runtime.cpp holds a
# HailoRT type. With the option OFF the stub answers instead, and selecting
# the device (GRPARSE_ORT_EP=hailo) fails startup.
option(GRPARSE_NATIVE_HAILO "Run the image models on a Hailo-10H through HailoRT" OFF)
set(GRPARSE_HAILORT_ROOT "" CACHE PATH "HailoRT install prefix (include/hailo, lib/libhailort.so)")

if(GRPARSE_NATIVE_HAILO)
  find_path(GRPARSE_HAILORT_INCLUDE hailo/hailort.hpp REQUIRED
    HINTS "${GRPARSE_HAILORT_ROOT}/include")
  find_library(GRPARSE_HAILORT_LIBRARY hailort REQUIRED
    HINTS "${GRPARSE_HAILORT_ROOT}/lib")
  target_sources(grparse_core PRIVATE src/hailo/hailo_runtime.cpp)
  # SYSTEM: HailoRT's headers are not held to this tree's warnings.
  target_include_directories(grparse_core SYSTEM PRIVATE "${GRPARSE_HAILORT_INCLUDE}")
  target_link_libraries(grparse_core PRIVATE "${GRPARSE_HAILORT_LIBRARY}")
else()
  target_sources(grparse_core PRIVATE src/hailo/hailo_runtime_stub.cpp)
endif()
