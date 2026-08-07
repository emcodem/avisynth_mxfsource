#include "net/http_byte_source.h"

#include <curl/curl.h>

#include <cctype>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <sstream>
#include <thread>

namespace mxfsrc {

namespace {

// Small forward seeks (the common case: a handful of jumps during Header
// Metadata parsing) are served by reading-and-discarding from the already
// open connection -- cheaper than paying for a new TLS handshake + request.
// Larger jumps (and any backward seek) restart the transfer with a new byte
// range instead.
constexpr uint64_t kSmallSeekThreshold = 256 * 1024;

// Bytes buffered ahead of the consumer before the background transfer's
// write callback blocks (backpressure) -- bounds memory use regardless of
// how far SourceEngine/EssenceWalker have gotten.
constexpr size_t kMaxBufferedBytes = 4 * 1024 * 1024;

constexpr int kMaxRetries = 2;  // up to 3 attempts total per logical transfer

std::once_flag g_curlInitFlag;

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

bool IsRetryableFailure(CURLcode rc, long httpCode) {
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

std::string DescribeFailure(CURLcode rc, long httpCode) {
  if (rc != CURLE_OK && rc != CURLE_WRITE_ERROR) {
    return std::string("HTTP transfer failed: ") + curl_easy_strerror(rc);
  }
  switch (httpCode) {
    case 403:
      return "HTTP 403 Forbidden (if this is an S3 presigned URL, it may have expired)";
    case 404:
      return "HTTP 404 Not Found";
    case 416:
      return "HTTP 416 Range Not Satisfiable";
    default:
      if (httpCode >= 400) return "HTTP " + std::to_string(httpCode) + " error";
      return {};
  }
}

void ApplyCommonOptions(CURL* easy, const std::string& url, curl_slist* headerList) {
  curl_easy_setopt(easy, CURLOPT_URL, url.c_str());
  curl_easy_setopt(easy, CURLOPT_HTTPGET, 1L);
  if (headerList) curl_easy_setopt(easy, CURLOPT_HTTPHEADER, headerList);
  curl_easy_setopt(easy, CURLOPT_FOLLOWLOCATION, 1L);
  curl_easy_setopt(easy, CURLOPT_MAXREDIRS, 5L);
  curl_easy_setopt(easy, CURLOPT_CONNECTTIMEOUT, 15L);
  curl_easy_setopt(easy, CURLOPT_LOW_SPEED_LIMIT, 512L);  // bytes/sec
  curl_easy_setopt(easy, CURLOPT_LOW_SPEED_TIME, 30L);    // seconds
  curl_easy_setopt(easy, CURLOPT_SSL_VERIFYPEER, 1L);
  curl_easy_setopt(easy, CURLOPT_SSL_VERIFYHOST, 2L);
  curl_easy_setopt(easy, CURLOPT_NOSIGNAL, 1L);
  curl_easy_setopt(easy, CURLOPT_TCP_KEEPALIVE, 1L);
}

}  // namespace

void EnsureCurlGlobalInit() {
  std::call_once(g_curlInitFlag, [] { curl_global_init(CURL_GLOBAL_DEFAULT); });
  // curl_global_cleanup() is deliberately never called -- see header comment:
  // this is a plugin loaded into an arbitrary host process, and tearing down
  // libcurl's global state on our own DLL unload could race any other user
  // of libcurl already loaded in that same process.
}

HttpByteSource::HttpByteSource(const std::string& url, const std::string& rawHeaders) : url_(url) {
  EnsureCurlGlobalInit();
  easy_ = curl_easy_init();
  if (!easy_) {
    lastError_ = "failed to initialize libcurl easy handle";
    return;
  }
  headerList_ = BuildHeaderList(rawHeaders);
  opened_ = ProbeSizeAndRangeSupport();
}

HttpByteSource::~HttpByteSource() {
  StopTransfer();
  if (easy_) curl_easy_cleanup(static_cast<CURL*>(easy_));
  if (headerList_) curl_slist_free_all(headerList_);
}

size_t HttpByteSource::HeaderCallback(char* buffer, size_t size, size_t nitems, void* userdata) {
  auto* self = static_cast<HttpByteSource*>(userdata);
  const size_t total = size * nitems;
  std::string line(buffer, total);

  auto ciStartsWith = [](const std::string& s, const char* prefix) {
    const size_t len = std::strlen(prefix);
    if (s.size() < len) return false;
    for (size_t i = 0; i < len; ++i) {
      if (std::tolower(static_cast<unsigned char>(s[i])) != std::tolower(static_cast<unsigned char>(prefix[i])))
        return false;
    }
    return true;
  };

  if (ciStartsWith(line, "content-range:")) {
    const size_t slash = line.find_last_of('/');
    if (slash != std::string::npos) {
      const std::string totalStr = line.substr(slash + 1);
      const uint64_t parsed = std::strtoull(totalStr.c_str(), nullptr, 10);
      if (parsed > 0) {
        self->totalSize_ = parsed;
        self->rangeSupported_ = true;
      }
    }
  } else if (ciStartsWith(line, "content-length:") && !self->rangeSupported_) {
    const size_t colon = line.find(':');
    if (colon != std::string::npos) {
      self->totalSize_ = std::strtoull(line.c_str() + colon + 1, nullptr, 10);
    }
  }
  return total;
}

bool HttpByteSource::ProbeSizeAndRangeSupport() {
  // A HEAD request is deliberately avoided: an S3 presigned URL is signed
  // for one specific HTTP method (typically GET), so HEAD against a
  // GET-signed URL returns 403 even though the URL is perfectly valid for
  // GET. Instead, issue a ranged GET for just the first byte and abort the
  // transfer the instant any body data starts arriving (via a write
  // callback that always returns 0) -- this way probing the size never
  // downloads a meaningful amount of the file even if the server ignores
  // the Range request and would otherwise have sent the whole body.
  auto abortAfterHeaders = [](char*, size_t, size_t, void*) -> size_t { return 0; };
  CURL* easy = static_cast<CURL*>(easy_);

  for (int attempt = 0; attempt <= kMaxRetries; ++attempt) {
    curl_easy_reset(easy);
    ApplyCommonOptions(easy, url_, headerList_);
    curl_easy_setopt(easy, CURLOPT_RANGE, "0-0");
    curl_easy_setopt(easy, CURLOPT_HEADERFUNCTION, &HttpByteSource::HeaderCallback);
    curl_easy_setopt(easy, CURLOPT_HEADERDATA, this);
    curl_easy_setopt(easy, CURLOPT_WRITEFUNCTION, +abortAfterHeaders);

    const CURLcode rc = curl_easy_perform(easy);
    long httpCode = 0;
    curl_easy_getinfo(easy, CURLINFO_RESPONSE_CODE, &httpCode);

    const bool ok = (rc == CURLE_OK || rc == CURLE_WRITE_ERROR) && (httpCode == 200 || httpCode == 206);
    if (ok) return true;
    if (!IsRetryableFailure(rc, httpCode) || attempt == kMaxRetries) {
      lastError_ = DescribeFailure(rc, httpCode);
      if (lastError_.empty()) lastError_ = "failed to open URL: " + url_;
      return false;
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(500 * (attempt + 1)));
  }
  return false;
}

size_t HttpByteSource::StreamingWriteCallback(char* ptr, size_t size, size_t nmemb, void* userdata) {
  auto* self = static_cast<HttpByteSource*>(userdata);
  const size_t n = size * nmemb;
  std::unique_lock<std::mutex> lock(self->mutex_);
  if (self->abortRequested_) return 0;
  self->cv_.wait(lock, [self] { return self->buffer_.size() < kMaxBufferedBytes || self->abortRequested_; });
  if (self->abortRequested_) return 0;
  self->buffer_.insert(self->buffer_.end(), ptr, ptr + n);
  self->deliveredThisTransfer_ += n;
  self->cv_.notify_all();
  return n;
}

void HttpByteSource::RunTransfer(uint64_t fromByte) {
  CURL* easy = static_cast<CURL*>(easy_);
  for (int attempt = 0; attempt <= kMaxRetries; ++attempt) {
    uint64_t rangeStart;
    {
      std::lock_guard<std::mutex> lock(mutex_);
      if (abortRequested_) {
        transferDone_ = true;
        cv_.notify_all();
        return;
      }
      // Resume from wherever the previous attempt left off (deliveredThis-
      // Transfer_ already reflects bytes appended to buffer_), not from
      // fromByte again -- otherwise a mid-transfer retry would duplicate
      // already-buffered bytes.
      rangeStart = fromByte + deliveredThisTransfer_;
    }
    const std::string rangeStr = std::to_string(rangeStart) + "-";

    curl_easy_reset(easy);
    ApplyCommonOptions(easy, url_, headerList_);
    if (rangeStart > 0) curl_easy_setopt(easy, CURLOPT_RANGE, rangeStr.c_str());
    curl_easy_setopt(easy, CURLOPT_WRITEFUNCTION, &HttpByteSource::StreamingWriteCallback);
    curl_easy_setopt(easy, CURLOPT_WRITEDATA, this);

    const CURLcode rc = curl_easy_perform(easy);
    long httpCode = 0;
    curl_easy_getinfo(easy, CURLINFO_RESPONSE_CODE, &httpCode);

    {
      std::lock_guard<std::mutex> lock(mutex_);
      if (abortRequested_) {
        transferDone_ = true;
        cv_.notify_all();
        return;
      }
      const bool ok = rc == CURLE_OK && (httpCode == 200 || httpCode == 206);
      if (ok) {
        transferDone_ = true;
        cv_.notify_all();
        return;
      }
      if (!IsRetryableFailure(rc, httpCode) || attempt == kMaxRetries) {
        transferError_ = true;
        transferErrorMsg_ = DescribeFailure(rc, httpCode);
        if (transferErrorMsg_.empty()) transferErrorMsg_ = "HTTP transfer failed";
        transferDone_ = true;
        cv_.notify_all();
        return;
      }
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(500 * (attempt + 1)));
  }
}

bool HttpByteSource::StartTransfer(uint64_t fromByte) {
  StopTransfer();
  {
    std::lock_guard<std::mutex> lock(mutex_);
    buffer_.clear();
    transferDone_ = false;
    transferError_ = false;
    transferErrorMsg_.clear();
    deliveredThisTransfer_ = 0;
    abortRequested_ = false;
  }
  // Every call site passes the byte offset that should become the new
  // logical read position -- EnsureStreamStarted() passes logicalPos_
  // itself (a no-op update), Seek()'s restart branches pass the seek
  // target, so this is always correct, not just for the lazy-start case.
  logicalPos_ = fromByte;
  worker_ = std::thread([this, fromByte] { RunTransfer(fromByte); });
  return true;
}

void HttpByteSource::StopTransfer() {
  if (!worker_.joinable()) return;
  {
    std::lock_guard<std::mutex> lock(mutex_);
    abortRequested_ = true;
    cv_.notify_all();
  }
  worker_.join();
}

bool HttpByteSource::EnsureStreamStarted() {
  if (worker_.joinable()) return true;
  return StartTransfer(logicalPos_);
}

bool HttpByteSource::FillAtLeast(size_t n) {
  if (!EnsureStreamStarted()) return false;
  std::unique_lock<std::mutex> lock(mutex_);
  cv_.wait(lock, [this, n] { return buffer_.size() >= n || transferDone_; });
  if (buffer_.size() >= n) return true;
  if (transferError_) lastError_ = transferErrorMsg_;
  return false;
}

bool HttpByteSource::ReadBytes(void* dst, size_t n) {
  if (n == 0) return true;
  if (!FillAtLeast(n)) return false;
  std::lock_guard<std::mutex> lock(mutex_);
  auto* out = static_cast<uint8_t*>(dst);
  for (size_t i = 0; i < n; ++i) {
    out[i] = buffer_.front();
    buffer_.pop_front();
  }
  logicalPos_ += n;
  cv_.notify_all();  // wake the writer if it was blocked on a full buffer
  return true;
}

bool HttpByteSource::DiscardBytes(uint64_t n) {
  uint8_t scratch[64 * 1024];
  while (n > 0) {
    const size_t chunk = static_cast<size_t>(n < sizeof(scratch) ? n : sizeof(scratch));
    if (!ReadBytes(scratch, chunk)) return false;
    n -= chunk;
  }
  return true;
}

bool HttpByteSource::Seek(uint64_t pos) {
  if (pos == logicalPos_) return true;

  if (pos > logicalPos_) {
    const uint64_t delta = pos - logicalPos_;
    if (!rangeSupported_ || delta <= kSmallSeekThreshold) {
      if (!rangeSupported_ && !loggedNoRangeFallback_ && delta > kSmallSeekThreshold) {
        loggedNoRangeFallback_ = true;
        std::fprintf(stderr,
                     "[MxfSource] warning: HTTP server for %s does not support byte-range requests; "
                     "seeking is emulated by discarding downloaded bytes and may be slow\n",
                     url_.c_str());
      }
      return DiscardBytes(delta);
    }
    return StartTransfer(pos);
  }

  // Backward seek.
  if (rangeSupported_) return StartTransfer(pos);

  // No range support at all: the only way to move backward is to restart
  // from byte 0 and discard forward up to `pos` -- always correct, just
  // slow, and surfaced once so the user understands why.
  if (!loggedNoRangeFallback_) {
    loggedNoRangeFallback_ = true;
    std::fprintf(stderr,
                 "[MxfSource] warning: HTTP server for %s does not support byte-range requests; "
                 "seeking is emulated by restarting the download from the beginning and may be slow\n",
                 url_.c_str());
  }
  if (!StartTransfer(0)) return false;
  return DiscardBytes(pos);
}

bool HttpByteSource::Skip(uint64_t n) { return Seek(logicalPos_ + n); }

}  // namespace mxfsrc
