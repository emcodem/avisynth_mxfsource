# Vendors prebuilt LGPL FFmpeg shared libraries (avcodec/avutil) via CMake
# FetchContent, so the build is self-contained (no system FFmpeg dev package
# required) without committing ~100MB of binaries into the git repo -- see
# plan Phase 3 notes. Source: BtbN/FFmpeg-Builds "latest" LGPL shared
# release (master branch, --disable-gpl config), same builds widely used by
# other open-source AviSynth+/VapourSynth plugins for exactly this purpose.
include(FetchContent)

if(WIN32)
  set(_mxfsrc_ffmpeg_url
      "https://github.com/BtbN/FFmpeg-Builds/releases/download/latest/ffmpeg-master-latest-win64-lgpl-shared.zip")
else()
  set(_mxfsrc_ffmpeg_url
      "https://github.com/BtbN/FFmpeg-Builds/releases/download/latest/ffmpeg-master-latest-linux64-lgpl-shared.tar.xz")
endif()

FetchContent_Declare(ffmpeg_prebuilt URL ${_mxfsrc_ffmpeg_url} DOWNLOAD_EXTRACT_TIMESTAMP TRUE)
FetchContent_MakeAvailable(ffmpeg_prebuilt)

set(FFMPEG_ROOT ${ffmpeg_prebuilt_SOURCE_DIR})

add_library(FFmpeg_avutil SHARED IMPORTED GLOBAL)
add_library(FFmpeg_avcodec SHARED IMPORTED GLOBAL)

if(WIN32)
  file(GLOB _mxfsrc_avutil_dll "${FFMPEG_ROOT}/bin/avutil-*.dll")
  file(GLOB _mxfsrc_avcodec_dll "${FFMPEG_ROOT}/bin/avcodec-*.dll")
  file(GLOB _mxfsrc_swresample_dll "${FFMPEG_ROOT}/bin/swresample-*.dll")
  list(GET _mxfsrc_avutil_dll 0 _mxfsrc_avutil_dll)
  list(GET _mxfsrc_avcodec_dll 0 _mxfsrc_avcodec_dll)
  list(GET _mxfsrc_swresample_dll 0 _mxfsrc_swresample_dll)
  set_target_properties(FFmpeg_avutil PROPERTIES
    IMPORTED_LOCATION "${_mxfsrc_avutil_dll}"
    IMPORTED_IMPLIB "${FFMPEG_ROOT}/lib/avutil.lib")
  set_target_properties(FFmpeg_avcodec PROPERTIES
    IMPORTED_LOCATION "${_mxfsrc_avcodec_dll}"
    IMPORTED_IMPLIB "${FFMPEG_ROOT}/lib/avcodec.lib")
  # avcodec-*.dll itself depends on swresample-*.dll in the vendored build
  # (verified via `dumpbin /dependents`), even for video-only decode use, so
  # it must be deployed alongside avcodec/avutil or the DLL loader fails
  # with STATUS_DLL_NOT_FOUND before our code ever runs.
  set(FFMPEG_RUNTIME_FILES "${_mxfsrc_avutil_dll}" "${_mxfsrc_avcodec_dll}" "${_mxfsrc_swresample_dll}")
else()
  file(GLOB _mxfsrc_avutil_so "${FFMPEG_ROOT}/lib/libavutil.so.*.*.*")
  file(GLOB _mxfsrc_avcodec_so "${FFMPEG_ROOT}/lib/libavcodec.so.*.*.*")
  file(GLOB _mxfsrc_swresample_so "${FFMPEG_ROOT}/lib/libswresample.so.*.*.*")
  list(GET _mxfsrc_avutil_so 0 _mxfsrc_avutil_so)
  list(GET _mxfsrc_avcodec_so 0 _mxfsrc_avcodec_so)
  list(GET _mxfsrc_swresample_so 0 _mxfsrc_swresample_so)
  set_target_properties(FFmpeg_avutil PROPERTIES IMPORTED_LOCATION "${_mxfsrc_avutil_so}")
  set_target_properties(FFmpeg_avcodec PROPERTIES IMPORTED_LOCATION "${_mxfsrc_avcodec_so}")
  set(FFMPEG_RUNTIME_FILES "${_mxfsrc_avutil_so}" "${_mxfsrc_avcodec_so}" "${_mxfsrc_swresample_so}")
endif()

set_target_properties(FFmpeg_avutil FFmpeg_avcodec PROPERTIES
  INTERFACE_INCLUDE_DIRECTORIES "${FFMPEG_ROOT}/include")
set_target_properties(FFmpeg_avcodec PROPERTIES INTERFACE_LINK_LIBRARIES FFmpeg_avutil)

# Deploys FFmpeg's runtime shared libraries next to `target`'s build output,
# both under their fully-versioned filename and (on Linux) the SONAME the
# dynamic loader actually resolves DT_NEEDED against. Copying rather than
# relying on the archive's own embedded symlinks because those aren't
# reliably preserved by every extraction environment (observed: WSL DrvFs
# mounts drop them); copying costs a little extra disk in the build tree but
# works everywhere.
function(mxfsrc_deploy_ffmpeg_runtime target)
  # Linux: libavcodec needs libswresample, which it finds only through a run
  # path that applies to dependencies of dependencies -- old-style DT_RPATH,
  # not the DT_RUNPATH current linkers write by default (the test
  # executables otherwise fail with "libswresample.so.7: cannot open shared
  # object file"). The plugins set the same for their $ORIGIN run path.
  if(NOT WIN32)
    target_link_options(${target} PRIVATE -Wl,--disable-new-dtags)
  endif()
  foreach(_file ${FFMPEG_RUNTIME_FILES})
    add_custom_command(TARGET ${target} POST_BUILD
      COMMAND ${CMAKE_COMMAND} -E copy_if_different "${_file}" "$<TARGET_FILE_DIR:${target}>/")
    if(NOT WIN32)
      get_filename_component(_name "${_file}" NAME)
      string(REGEX MATCH "^(lib[a-z]+\\.so\\.[0-9]+)" _soname "${_name}")
      if(_soname)
        add_custom_command(TARGET ${target} POST_BUILD
          COMMAND ${CMAKE_COMMAND} -E copy_if_different "${_file}" "$<TARGET_FILE_DIR:${target}>/${_soname}")
      endif()
    endif()
  endforeach()
endfunction()
