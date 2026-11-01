add_library(mxfsource_avs SHARED
  avs_adapter.cpp
)

target_link_libraries(mxfsource_avs PRIVATE mxf_core)
target_include_directories(mxfsource_avs PRIVATE
  ${CMAKE_SOURCE_DIR}/third_party/avisynth
)
mxfsrc_deploy_ffmpeg_runtime(mxfsource_avs)

set_target_properties(mxfsource_avs PROPERTIES
  PREFIX ""
  OUTPUT_NAME "mxfsource"
)

if(NOT WIN32)
  set_target_properties(mxfsource_avs PROPERTIES OUTPUT_NAME "mxfsource_avs")
endif()

# Linux: find the bundled FFmpeg libs next to the plugin. DT_RPATH (not
# RUNPATH) is used so it also applies to libavcodec's own dependency on
# libswresample, which has no run path of its own.
if(NOT WIN32)
  set_target_properties(mxfsource_avs PROPERTIES
    BUILD_WITH_INSTALL_RPATH ON
    INSTALL_RPATH "$ORIGIN")
  target_link_options(mxfsource_avs PRIVATE -Wl,--disable-new-dtags)
endif()
