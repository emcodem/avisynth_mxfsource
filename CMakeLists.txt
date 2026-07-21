cmake_minimum_required(VERSION 3.24)
project(avs_mxf_plugin CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
# mxf_core is a static library consumed by both executables (tests) and the
# two adapter .so/.dll targets; on Linux, code linked into a shared object
# must be position-independent, so build all of it as PIC uniformly rather
# than special-casing per target (a no-op on Windows/MSVC).
set(CMAKE_POSITION_INDEPENDENT_CODE ON)

option(BUILD_AVS_PLUGIN "Build the AviSynth+ adapter (mxfsource_avs)" ON)
option(BUILD_VS_PLUGIN "Build the VapourSynth adapter (mxfsource_vs)" ON)
option(BUILD_TESTS "Build standalone MXF parser tests" ON)

if(MSVC)
  add_compile_options(/W4)
else()
  add_compile_options(-Wall -Wextra)
endif()

include(cmake/FetchFFmpeg.cmake)
find_package(Threads REQUIRED)

# --- shared host-agnostic core (mxf parser, codec decode, ring buffer engine) ---
add_library(mxf_core STATIC
  src/mxf/klv.cpp
  src/mxf/klv.h
  src/mxf/partition.cpp
  src/mxf/partition.h
  src/mxf/metadata_sets.cpp
  src/mxf/metadata_sets.h
  src/mxf/header_parser.cpp
  src/mxf/header_parser.h
  src/mxf/essence_walker.cpp
  src/mxf/essence_walker.h
  src/mxf/ul_tables.h
  src/codec/decoder.h
  src/codec/raw_unpack.cpp
  src/codec/raw_unpack.h
  src/codec/avcodec_decoder.cpp
  src/codec/avcodec_decoder.h
  src/codec/decoder_factory.cpp
  src/codec/decoder_factory.h
  src/core/frame_buffer.h
  src/core/options.h
  src/core/source_engine.cpp
  src/core/source_engine.h
)
target_include_directories(mxf_core PUBLIC src)
target_link_libraries(mxf_core PUBLIC FFmpeg_avcodec Threads::Threads)

if(BUILD_AVS_PLUGIN)
  add_subdirectory(src/avs)
endif()

if(BUILD_VS_PLUGIN)
  add_subdirectory(src/vs)
endif()

if(BUILD_TESTS)
  enable_testing()
  add_subdirectory(tests/mxf_parser_tests)
endif()
