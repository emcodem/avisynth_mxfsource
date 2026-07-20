cmake_minimum_required(VERSION 3.24)
project(avs_mxf_plugin CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

option(BUILD_AVS_PLUGIN "Build the AviSynth+ adapter (mxfsource_avs)" ON)
option(BUILD_VS_PLUGIN "Build the VapourSynth adapter (mxfsource_vs)" ON)
option(BUILD_TESTS "Build standalone MXF parser tests" ON)

if(MSVC)
  add_compile_options(/W4)
else()
  add_compile_options(-Wall -Wextra)
endif()

include(cmake/FetchFFmpeg.cmake)

# --- shared host-agnostic core (mxf parser, codec decode, ring buffer engine) ---
# Gains src/core (Phase 4) sources as those land.
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
)
target_include_directories(mxf_core PUBLIC src)
target_link_libraries(mxf_core PUBLIC FFmpeg_avcodec)

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
