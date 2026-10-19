// Unit tests for mxf/file_open.h: paths longer than Windows' MAX_PATH (260)
// and non-ASCII names must open, both through OpenFile directly and through
// FileByteSource (what MxfSource uses for the source file). Seven real files
// on the sample share failed to open with "failed to open file" only because
// their UNC path was 303 characters long.
#include <filesystem>
#include <iostream>
#include <string>

#include "mxf/file_byte_source.h"
#include "mxf/file_open.h"

using namespace mxfsrc;
namespace fs = std::filesystem;

namespace {

int g_failures = 0;

#define CHECK(cond)                                                             \
  do {                                                                          \
    if (!(cond)) {                                                              \
      std::cerr << "CHECK FAILED: " #cond " at " << __FILE__ << ":" << __LINE__ \
                << std::endl;                                                   \
      ++g_failures;                                                             \
    }                                                                           \
  } while (0)

// The directory tree itself is created through the extended-length form on
// Windows, since std::filesystem there is subject to MAX_PATH just like
// fopen -- the point of the test is that OpenFile copes with the *plain*
// long path a user would pass.
fs::path ForCreation(const fs::path& p) {
#if defined(_WIN32)
  return fs::path(L"\\\\?\\" + fs::absolute(p).wstring());
#else
  return p;
#endif
}

void TestRoundTrip(const fs::path& dir, const std::string& leafUtf8) {
  std::error_code ec;
  fs::create_directories(ForCreation(dir), ec);
  CHECK(!ec);
  const std::string path = (dir / fs::u8path(leafUtf8)).u8string();
  const std::string payload = "mxfsrc long path test";

  FILE* out = OpenFile(path, "wb");
  CHECK(out != nullptr);
  if (out) {
    CHECK(std::fwrite(payload.data(), 1, payload.size(), out) == payload.size());
    std::fclose(out);
  }

  FileByteSource source(path);
  CHECK(source.IsOpen());
  CHECK(source.Size() == payload.size());
  std::string back(payload.size(), '\0');
  CHECK(source.ReadBytes(back.data(), back.size()));
  CHECK(back == payload);
}

void TestPathLongerThanMaxPath() {
  const fs::path root = fs::temp_directory_path() / "mxfsrc_long_path_test";
  fs::path dir = root;
  while (dir.u8string().size() < 320) dir /= "a_rather_long_directory_name_0123456789";
  TestRoundTrip(dir, "clip.mxf");
  CHECK(dir.u8string().size() >= 320);
  std::error_code ec;
  fs::remove_all(ForCreation(root), ec);
}

void TestNonAsciiName() {
  const fs::path root = fs::temp_directory_path() / "mxfsrc_non_ascii_test";
  TestRoundTrip(root / fs::u8path(u8"Fußball_Ü-Wagen"), u8"wörthersee.mxf");
  std::error_code ec;
  fs::remove_all(root, ec);
}

void TestMissingFileFails() {
  CHECK(OpenFile((fs::temp_directory_path() / "mxfsrc_does_not_exist.mxf").u8string(), "rb") == nullptr);
  CHECK(!FileByteSource((fs::temp_directory_path() / "mxfsrc_does_not_exist.mxf").u8string()).IsOpen());
}

}  // namespace

int main() {
  TestPathLongerThanMaxPath();
  TestNonAsciiName();
  TestMissingFileFails();

  if (g_failures == 0) {
    std::cout << "all mxf file_open tests passed" << std::endl;
    return 0;
  }
  std::cerr << g_failures << " check(s) failed" << std::endl;
  return 1;
}
