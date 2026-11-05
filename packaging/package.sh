#!/usr/bin/env bash
# Packages the AviSynth and VapourSynth plugins of a finished build into
# dist/, the way releases ship them:
#   mxfsource-avisynth-<version>-<platform>.(zip|tar.gz)
#   mxfsource-vapoursynth-<version>-<platform>.(zip|tar.gz)
# each with the plugin, the bundled FFmpeg runtime (avcodec, avutil,
# swresample), FFmpeg-LICENSE.txt and NOTICE.txt.
#
#   packaging/package.sh <build dir> <platform: win64 | linux-x86_64>
#
# <version> is `git describe --tags --always --dirty`: the tag itself on a
# release tag (e.g. v0.2.0-beta.1).
set -euo pipefail

build=$1
platform=$2
version=$(git describe --tags --always --dirty)
root=$(pwd)
license="$build/_deps/ffmpeg_prebuilt-src/LICENSE.txt"
rm -rf "$root/dist" "$root/stage"
mkdir -p "$root/dist"

package() {
  local host=$1 dir=$2 plugin=$3
  local name="mxfsource-$host-$version-$platform"
  local stage="$root/stage/$name"
  mkdir -p "$stage"
  cp "$dir/$plugin" "$stage/"
  cp "$license" "$stage/FFmpeg-LICENSE.txt"
  if [[ $platform == win64 ]]; then
    cp "$dir"/avcodec-*.dll "$dir"/avutil-*.dll "$dir"/swresample-*.dll "$stage/"
    cp "$root/packaging/NOTICE-windows.txt" "$stage/NOTICE.txt"
    (cd "$stage" && 7z a -tzip -bso0 "$root/dist/$name.zip" .)
  else
    # The plugin's $ORIGIN run path finds the libraries under their SONAME
    # (libavcodec.so.63); the fully versioned copies aren't needed.
    local lib
    for lib in avcodec avutil swresample; do
      find "$dir" -maxdepth 1 -regex ".*/lib$lib\.so\.[0-9]+" -exec cp {} "$stage/" \;
    done
    cp "$root/packaging/NOTICE-linux.txt" "$stage/NOTICE.txt"
    tar -czf "$root/dist/$name.tar.gz" -C "$stage" .
  fi
  echo "packaged $name:"
  ls -l "$stage"
}

if [[ $platform == win64 ]]; then
  package avisynth "$build/src/avs/Release" mxfsource.dll
  package vapoursynth "$build/src/vs/Release" mxfsource_vs.dll
else
  package avisynth "$build/src/avs" mxfsource_avs.so
  package vapoursynth "$build/src/vs" mxfsource_vs.so
fi
ls -l "$root/dist"
