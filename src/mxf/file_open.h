#pragma once

#include <cstdio>
#include <string>

namespace mxfsrc {

// fopen() replacement used for every local file the plugin opens (source
// MXF, logfile, index JSON).
//
// On Windows, plain fopen() goes through the ANSI API, which fails for paths
// of MAX_PATH (260) characters or more -- observed on 7 real files on the
// sample share -- and can't open names outside the ANSI codepage. The
// process-wide fix (a longPathAware manifest) only works in the host .exe's
// manifest, never a plugin DLL's, so instead this converts the path to UTF-16
// and, when it is that long, opens it as an extended-length "\\?\" (local) or
// "\\?\UNC\" (network share) path, which has no MAX_PATH limit and needs no
// manifest or registry setting.
//
// The input is decoded as UTF-8 (what VapourSynth passes) if it is valid
// UTF-8, otherwise in the ANSI codepage (what AviSynth+ scripts normally
// carry).
//
// Elsewhere this is plain fopen(): Linux has no comparable limit (PATH_MAX
// is 4096) and paths are byte strings.
FILE* OpenFile(const std::string& path, const char* mode);

}  // namespace mxfsrc
