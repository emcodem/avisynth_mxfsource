cmake_minimum_required(VERSION 3.24)

# Must be set before project() so it governs the default /MD-vs-/MT flags
# CMake initializes at that point. Statically linking the MSVC C++ runtime
# means mxfsource.dll/mxfsource_vs.dll carry their own copy of the CRT
# instead of dynamically depending on MSVCP140.dll/VCRUNTIME140*.dll --
# this plugin gets loaded into arbitrary, unpredictable host processes
# (AviSynth+/VapourSynth hosts embedded in third-party applications), and a
# host whose own executable directory happens to bundle an older/ABI-
# incompatible MSVCP140.dll will shadow ours in the DLL search order
# (main-executable directory is searched before System32), corrupting
# memory the moment our code touches std::string/shared_ptr/mutex/thread.
# Confirmed as the exact root cause of a real crash report: the host's
# bundled MSVCP140.dll (14.24, ~2019) predated the ABI our code was built
# against (MSVC 14.42/VS2022), while System32's newer copy (14.51) worked
# fine. No FFmpeg DLL is affected -- those are MinGW-built and never
# touch MSVCP140.dll at all.
cmake_policy(SET CMP0091 NEW)
set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")

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
include(cmake/FetchCurl.cmake)
find_package(Threads REQUIRED)

# --- shared host-agnostic core (mxf parser, codec decode, ring buffer engine) ---
add_library(mxf_core STATIC
  src/mxf/klv.cpp
  src/mxf/klv.h
  src/mxf/file_byte_source.cpp
  src/mxf/file_open.cpp
  src/mxf/integrity.cpp
  src/mxf/integrity.h
  src/mxf/file_byte_source.h
  src/net/http_byte_source.cpp
  src/net/http_byte_source.h
  src/net/http_put.cpp
  src/net/http_put.h
  src/mxf/partition.cpp
  src/mxf/partition.h
  src/mxf/metadata_sets.cpp
  src/mxf/metadata_sets.h
  src/mxf/header_parser.cpp
  src/mxf/header_parser.h
  src/mxf/essence_walker.cpp
  src/mxf/essence_walker.h
  src/mxf/index_table.cpp
  src/mxf/index_table.h
  src/mxf/memory_byte_source.h
  src/mxf/index_serialization.cpp
  src/mxf/index_serialization.h
  src/mxf/index_transport.cpp
  src/mxf/index_transport.h
  src/mxf/mpeg2_bitstream.cpp
  src/mxf/mpeg2_bitstream.h
  src/mxf/h264_bitstream.cpp
  src/mxf/h264_bitstream.h
  src/mxf/bitstream_index.cpp
  src/mxf/bitstream_index.h
  src/mxf/audio_sample_index.cpp
  src/mxf/audio_sample_index.h
  src/mxf/ul_tables.h
  src/codec/decoder.h
  src/codec/raw_unpack.cpp
  src/codec/raw_unpack.h
  src/codec/avcodec_decoder.cpp
  src/codec/avcodec_decoder.h
  src/codec/decoder_factory.cpp
  src/codec/decoder_factory.h
  src/codec/pcm_interleave.cpp
  src/codec/pcm_interleave.h
  src/core/frame_buffer.h
  src/core/options.h
  src/core/use_audio_mode.h
  src/core/index_extract.cpp
  src/core/index_policy.cpp
  src/core/index_policy.h
  src/core/index_extract.h
  src/core/source_engine.cpp
  src/core/source_engine.h
)
target_include_directories(mxf_core PUBLIC src)
target_link_libraries(mxf_core PUBLIC FFmpeg_avcodec Threads::Threads CURL::libcurl)

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
