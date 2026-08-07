#include "net/http_put.h"

#include <curl/curl.h>

#include <cstring>

#include "net/http_byte_source.h"

namespace mxfsrc {

namespace {

constexpr long kConnectTimeoutSeconds = 15L;
constexpr long kLowSpeedLimitBytesPerSec = 512L;
constexpr long kLowSpeedTimeSeconds = 30L;

struct UploadState {
  const uint8_t* data;
  size_t size;
  size_t offset = 0;
};

size_t UploadReadCallback(char* dst, size_t size, size_t nmemb, void* userdata) {
  auto* state = static_cast<UploadState*>(userdata);
  const size_t want = size * nmemb;
  const size_t remaining = state->size - state->offset;
  const size_t n = want < remaining ? want : remaining;
  std::memcpy(dst, state->data + state->offset, n);
  state->offset += n;
  return n;
}

size_t DownloadWriteCallback(char* ptr, size_t size, size_t nmemb, void* userdata) {
  auto* out = static_cast<std::vector<uint8_t>*>(userdata);
  const size_t n = size * nmemb;
  out->insert(out->end(), ptr, ptr + n);
  return n;
}

std::string DescribeCurlFailure(CURLcode rc, long httpCode) {
  if (rc != CURLE_OK) return std::string("HTTP transfer failed: ") + curl_easy_strerror(rc);
  if (httpCode == 403) {
    return "HTTP 403 Forbidden (if this is a presigned URL, it may have expired or be signed for a different HTTP "
           "method -- an S3 PUT-presigned URL cannot be used for GET or vice versa)";
  }
  if (httpCode == 404) return "HTTP 404 Not Found";
  if (httpCode >= 400) return "HTTP " + std::to_string(httpCode) + " error";
  return {};
}

void ApplyCommonOptions(CURL* easy, const std::string& url) {
  curl_easy_setopt(easy, CURLOPT_URL, url.c_str());
  curl_easy_setopt(easy, CURLOPT_CONNECTTIMEOUT, kConnectTimeoutSeconds);
  curl_easy_setopt(easy, CURLOPT_LOW_SPEED_LIMIT, kLowSpeedLimitBytesPerSec);
  curl_easy_setopt(easy, CURLOPT_LOW_SPEED_TIME, kLowSpeedTimeSeconds);
  curl_easy_setopt(easy, CURLOPT_SSL_VERIFYPEER, 1L);
  curl_easy_setopt(easy, CURLOPT_SSL_VERIFYHOST, 2L);
  curl_easy_setopt(easy, CURLOPT_NOSIGNAL, 1L);
  curl_easy_setopt(easy, CURLOPT_FOLLOWLOCATION, 1L);
  curl_easy_setopt(easy, CURLOPT_MAXREDIRS, 5L);
}

}  // namespace

bool HttpPutBlob(const std::string& url, const std::vector<uint8_t>& data, std::string* errorOut) {
  EnsureCurlGlobalInit();
  CURL* easy = curl_easy_init();
  if (!easy) {
    if (errorOut) *errorOut = "failed to initialize libcurl easy handle";
    return false;
  }

  UploadState state{data.data(), data.size()};
  ApplyCommonOptions(easy, url);
  curl_easy_setopt(easy, CURLOPT_UPLOAD, 1L);
  curl_easy_setopt(easy, CURLOPT_READFUNCTION, UploadReadCallback);
  curl_easy_setopt(easy, CURLOPT_READDATA, &state);
  curl_easy_setopt(easy, CURLOPT_INFILESIZE_LARGE, static_cast<curl_off_t>(data.size()));

  const CURLcode rc = curl_easy_perform(easy);
  long httpCode = 0;
  curl_easy_getinfo(easy, CURLINFO_RESPONSE_CODE, &httpCode);
  curl_easy_cleanup(easy);

  const bool ok = rc == CURLE_OK && httpCode >= 200 && httpCode < 300;
  if (!ok && errorOut) *errorOut = DescribeCurlFailure(rc, httpCode);
  return ok;
}

std::optional<std::vector<uint8_t>> HttpGetBlob(const std::string& url, std::string* errorOut) {
  EnsureCurlGlobalInit();
  CURL* easy = curl_easy_init();
  if (!easy) {
    if (errorOut) *errorOut = "failed to initialize libcurl easy handle";
    return std::nullopt;
  }

  std::vector<uint8_t> body;
  ApplyCommonOptions(easy, url);
  curl_easy_setopt(easy, CURLOPT_HTTPGET, 1L);
  curl_easy_setopt(easy, CURLOPT_WRITEFUNCTION, DownloadWriteCallback);
  curl_easy_setopt(easy, CURLOPT_WRITEDATA, &body);

  const CURLcode rc = curl_easy_perform(easy);
  long httpCode = 0;
  curl_easy_getinfo(easy, CURLINFO_RESPONSE_CODE, &httpCode);
  curl_easy_cleanup(easy);

  if (!(rc == CURLE_OK && httpCode >= 200 && httpCode < 300)) {
    if (errorOut) *errorOut = DescribeCurlFailure(rc, httpCode);
    return std::nullopt;
  }
  return body;
}

}  // namespace mxfsrc
