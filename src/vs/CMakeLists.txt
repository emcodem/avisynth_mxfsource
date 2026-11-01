add_library(mxfsource_vs SHARED
  vs_adapter.cpp
)

target_link_libraries(mxfsource_vs PRIVATE mxf_core)
target_include_directories(mxfsource_vs PRIVATE
  ${CMAKE_SOURCE_DIR}/third_party/vapoursynth
)
mxfsrc_deploy_ffmpeg_runtime(mxfsource_vs)

set_target_properties(mxfsource_vs PROPERTIES
  PREFIX ""
  OUTPUT_NAME "mxfsource_vs"
)

# Linux: find the bundled FFmpeg libs next to the plugin. DT_RPATH (not
# RUNPATH) is used so it also applies to libavcodec's own dependency on
# libswresample, which has no run path of its own.
if(NOT WIN32)
  set_target_properties(mxfsource_vs PROPERTIES
    BUILD_WITH_INSTALL_RPATH ON
    INSTALL_RPATH "$ORIGIN")
  target_link_options(mxfsource_vs PRIVATE -Wl,--disable-new-dtags)
endif()
