#pragma once

#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <deque>
#include <mutex>
#include <string>
#include <thread>

#include "mxf/klv.h"

struct curl_slist;

namespace mxfsrc {

// IByteSource backend that streams an MXF file over HTTP/HTTPS via libcurl --
// this is what lets MxfSource(url) work against a plain HTTP(S) link or an
// S3 presigned URL exactly like it works against a local path.
//
// One curl "easy" handle is owned per instance and reused across every
// request this instance ever makes (its own connection-keepalive cache), but
// never touched by more than one thread at a time: the background streaming
// transfer always runs on a worker thread that is fully joined before the
// handle is reconfigured and reused for the next request.
//
// Reads are mostly sequential (matches EssenceWalker's forward-only walk and
// the decode thread's continuous forward reads) with occasional small
// forward seeks during header parsing and occasional large jumps from
// SourceEngine::SeekAndDecode's fallback path. Seek() picks between two
// strategies accordingly -- see .cpp for the threshold/rationale.
class HttpByteSource : public IByteSource {
 public:
  // `rawHeaders` is a "Name: value\r\nName2: value2" block (may be empty);
  // ignored characters/lines are tolerated defensively but callers should
  // pass well-formed header lines (see OpenOptions::http_headers).
  HttpByteSource(const std::string& url, const std::string& rawHeaders);
  ~HttpByteSource() override;
  HttpByteSource(const HttpByteSource&) = delete;
  HttpByteSource& operator=(const HttpByteSource&) = delete;

  bool IsOpen() const override { return opened_; }
  uint64_t Tell() const override { return logicalPos_; }
  bool Seek(uint64_t pos) override;
  uint64_t Size() const override { return totalSize_; }
  bool ReadBytes(void* dst, size_t n) override;
  bool Skip(uint64_t n) override;
  std::string LastError() const override { return lastError_; }

 private:
  bool ProbeSizeAndRangeSupport();
  bool DiscardBytes(uint64_t n);
  bool EnsureStreamStarted();
  void StopTransfer();
  bool StartTransfer(uint64_t fromByte);
  void RunTransfer(uint64_t fromByte);
  bool FillAtLeast(size_t n);

  static size_t StreamingWriteCallback(char* ptr, size_t size, size_t nmemb, void* userdata);
  static size_t HeaderCallback(char* buffer, size_t size, size_t nitems, void* userdata);

  std::string url_;
  curl_slist* headerList_ = nullptr;
  void* easy_ = nullptr;  // CURL*; kept as void* so <curl/curl.h> is only needed in the .cpp

  uint64_t totalSize_ = 0;
  bool rangeSupported_ = false;
  bool opened_ = false;
  bool loggedNoRangeFallback_ = false;
  std::string lastError_;

  // Guards buffer_/transferDone_/transferError_/transferErrorMsg_/
  // deliveredThisTransfer_ -- written by the worker thread inside
  // StreamingWriteCallback/RunTransfer, read/consumed by ReadBytes on the
  // calling thread.
  std::mutex mutex_;
  std::condition_variable cv_;
  std::deque<uint8_t> buffer_;
  bool transferDone_ = false;
  bool transferError_ = false;
  std::string transferErrorMsg_;
  uint64_t deliveredThisTransfer_ = 0;
  std::atomic<bool> abortRequested_{false};

  uint64_t logicalPos_ = 0;  // next byte ReadBytes() will return
  std::thread worker_;
};

}  // namespace mxfsrc
