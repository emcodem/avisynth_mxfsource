#include "mxf/file_open.h"

#if defined(_WIN32)
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>

#include <vector>
#endif

namespace mxfsrc {

#if defined(_WIN32)

namespace {

std::wstring Widen(const std::string& s) {
  if (s.empty()) return {};
  // Valid UTF-8 first (MB_ERR_INVALID_CHARS rejects anything else), then the
  // ANSI codepage -- a non-ASCII ANSI string is practically never valid UTF-8.
  for (const UINT codePage : {static_cast<UINT>(CP_UTF8), static_cast<UINT>(CP_ACP)}) {
    const DWORD flags = codePage == CP_UTF8 ? MB_ERR_INVALID_CHARS : 0;
    const int len = MultiByteToWideChar(codePage, flags, s.data(), static_cast<int>(s.size()), nullptr, 0);
    if (len <= 0) continue;
    std::wstring out(static_cast<size_t>(len), L'\0');
    MultiByteToWideChar(codePage, flags, s.data(), static_cast<int>(s.size()), out.data(), len);
    return out;
  }
  return {};
}

bool StartsWith(const std::wstring& s, const wchar_t* prefix) { return s.rfind(prefix, 0) == 0; }

// Turns a too-long path into its extended-length form. "\\?\" paths are
// passed to the filesystem verbatim (no "/" -> "\" or "." / ".."
// normalization, no relative-path resolution), so the path is made absolute
// and normalized with GetFullPathNameW first.
std::wstring ToExtendedLengthPath(const std::wstring& path) {
  if (StartsWith(path, L"\\\\?\\") || StartsWith(path, L"\\\\.\\")) return path;
  const DWORD needed = GetFullPathNameW(path.c_str(), 0, nullptr, nullptr);
  if (needed == 0) return path;
  std::vector<wchar_t> buf(needed);
  const DWORD written = GetFullPathNameW(path.c_str(), needed, buf.data(), nullptr);
  if (written == 0 || written >= needed) return path;
  const std::wstring full(buf.data(), written);
  if (StartsWith(full, L"\\\\")) return L"\\\\?\\UNC\\" + full.substr(2);  // \\server\share\...
  return L"\\\\?\\" + full;
}

}  // namespace

FILE* OpenFile(const std::string& path, const char* mode) {
  std::wstring wpath = Widen(path);
  const std::wstring wmode = Widen(mode);
  if (wpath.empty() || wmode.empty()) return nullptr;
  // Short paths are opened as given, so device names (NUL, CON) and every
  // path that already worked behave exactly as before.
  if (wpath.size() >= MAX_PATH) wpath = ToExtendedLengthPath(wpath);
  return _wfopen(wpath.c_str(), wmode.c_str());
}

#else

FILE* OpenFile(const std::string& path, const char* mode) { return std::fopen(path.c_str(), mode); }

#endif

}  // namespace mxfsrc
