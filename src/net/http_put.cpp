#include "net/http_put.h"

#include <curl/curl.h>

#include <chrono>
#include <cstring>
#include <sstream>
#include <thread>

#include "net/http_byte_source.h"

namespace mxfsrc {

namespace {

constexpr long kConnectTimeoutSeconds = 15L;
constexpr long kLowSpeedLimitBytesPerSec = 512L;
constexpr long kLowSpeedTimeSeconds = 30L;
constexpr int kRangeMaxRetries = 2;  // up to 3 attempts total per range fetch

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

// Duplicated in miniature from http_byte_source.cpp's own BuildHeaderList/
// IsRetryableFailure rather than shared: both are a handful of lines, and
// each already has its own separately-worded DescribeCurlFailure/
// DescribeFailure for the same reason (this file's error text is written
// for a one-shot blob/range operation, not a persistent stream).
curl_slist* BuildHeaderList(const std::string& rawHeaders) {
  curl_slist* list = nullptr;
  std::istringstream stream(rawHeaders);
  std::string line;
  while (std::getline(stream, line)) {
    while (!line.empty() && (line.back() == '\r' || line.back() == '\n')) line.pop_back();
    if (!line.empty()) list = curl_slist_append(list, line.c_str());
  }
  return list;
}

bool IsRetryableRangeFailure(CURLcode rc, long httpCode) {
  if (rc == CURLE_OK) return httpCode >= 500 && httpCode < 600;
  switch (rc) {
    case CURLE_COULDNT_CONNECT:
    case CURLE_COULDNT_RESOLVE_HOST:
    case CURLE_COULDNT_RESOLVE_PROXY:
    case CURLE_OPERATION_TIMEDOUT:
    case CURLE_RECV_ERROR:
    case CURLE_SEND_ERROR:
    case CURLE_SSL_CONNECT_ERROR:
    case CURLE_GOT_NOTHING:
      return true;
    default:
      return false;
  }
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

std::optional<std::vector<uint8_t>> HttpGetRange(const std::string& url, const std::string& httpHeaders,
                                                  uint64_t offset, uint64_t length, std::string* errorOut) {
  if (length == 0) return std::vector<uint8_t>{};

  EnsureCurlGlobalInit();
  CURL* easy = curl_easy_init();
  if (!easy) {
    if (errorOut) *errorOut = "failed to initialize libcurl easy handle";
    return std::nullopt;
  }
  curl_slist* headerList = BuildHeaderList(httpHeaders);
  const std::string rangeStr = std::to_string(offset) + "-" + std::to_string(offset + length - 1);

  std::optional<std::vector<uint8_t>> result;
  for (int attempt = 0; attempt <= kRangeMaxRetries; ++attempt) {
    std::vector<uint8_t> body;
    curl_easy_reset(easy);
    ApplyCommonOptions(easy, url);
    if (headerList) curl_easy_setopt(easy, CURLOPT_HTTPHEADER, headerList);
    curl_easy_setopt(easy, CURLOPT_HTTPGET, 1L);
    curl_easy_setopt(easy, CURLOPT_RANGE, rangeStr.c_str());
    curl_easy_setopt(easy, CURLOPT_WRITEFUNCTION, DownloadWriteCallback);
    curl_easy_setopt(easy, CURLOPT_WRITEDATA, &body);

    const CURLcode rc = curl_easy_perform(easy);
    long httpCode = 0;
    curl_easy_getinfo(easy, CURLINFO_RESPONSE_CODE, &httpCode);

    if (rc == CURLE_OK && (httpCode == 200 || httpCode == 206)) {
      result = std::move(body);
      break;
    }
    if (!IsRetryableRangeFailure(rc, httpCode) || attempt == kRangeMaxRetries) {
      if (errorOut) *errorOut = DescribeCurlFailure(rc, httpCode);
      break;
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(500 * (attempt + 1)));
  }

  curl_easy_cleanup(easy);
  if (headerList) curl_slist_free_all(headerList);
  return result;
}

}  // namespace mxfsrc
