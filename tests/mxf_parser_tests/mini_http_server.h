#pragma once

// Minimal, single-purpose HTTP/1.1 loopback server used only by
// http_byte_source_test.cpp / http_source_engine_test.cpp to exercise
// HttpByteSource (src/net/http_byte_source.h) against real socket I/O
// without depending on any external server or live network access. Serves
// exactly one in-memory byte buffer, understands a single-range `Range:
// bytes=start-end` request, and can simulate a handful of failure modes
// (ignoring Range entirely, forcing an HTTP error status, or dropping the
// first N connections) that real HTTP servers / S3 can exhibit.
#include <atomic>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <map>
#include <mutex>
#include <sstream>
#include <string>
#include <thread>
#include <vector>

#if defined(_WIN32)
#include <winsock2.h>
#include <ws2tcpip.h>
#pragma comment(lib, "ws2_32.lib")
#else
#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>
#endif

namespace mxfsrc_test {

#if defined(_WIN32)
using SocketHandle = SOCKET;
constexpr SocketHandle kInvalidSocket = INVALID_SOCKET;
inline void CloseSocketHandle(SocketHandle s) { closesocket(s); }
#else
using SocketHandle = int;
constexpr SocketHandle kInvalidSocket = -1;
inline void CloseSocketHandle(SocketHandle s) { close(s); }
#endif

class MiniHttpServer {
 public:
  explicit MiniHttpServer(std::vector<uint8_t> body) : body_(std::move(body)) {
#if defined(_WIN32)
    WSADATA wsaData;
    WSAStartup(MAKEWORD(2, 2), &wsaData);
#endif
    listenFd_ = socket(AF_INET, SOCK_STREAM, 0);
    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    addr.sin_port = 0;  // let the OS pick a free port
    bind(listenFd_, reinterpret_cast<sockaddr*>(&addr), sizeof(addr));
    listen(listenFd_, 16);

    sockaddr_in bound{};
#if defined(_WIN32)
    int boundLen = sizeof(bound);
#else
    socklen_t boundLen = sizeof(bound);
#endif
    getsockname(listenFd_, reinterpret_cast<sockaddr*>(&bound), &boundLen);
    port_ = ntohs(bound.sin_port);

    thread_ = std::thread([this] { ServeLoop(); });
  }

  ~MiniHttpServer() {
    stop_ = true;
    CloseSocketHandle(listenFd_);
    if (thread_.joinable()) thread_.join();
#if defined(_WIN32)
    WSACleanup();
#endif
  }

  MiniHttpServer(const MiniHttpServer&) = delete;
  MiniHttpServer& operator=(const MiniHttpServer&) = delete;

  std::string BaseUrl() const { return "http://127.0.0.1:" + std::to_string(port_); }

  // If false, Range headers are ignored entirely and every request gets a
  // full 200 response -- exercises HttpByteSource's non-range-capable-
  // server fallback path.
  void SetRangeSupport(bool supported) { rangeSupported_ = supported; }

  // 0 = normal handling; otherwise every request gets this HTTP status with
  // an empty body (e.g. 403 to simulate an expired S3 presigned URL).
  void SetForceStatus(int status) { forceStatus_ = status; }

  // The first N accepted connections are closed immediately without any
  // response, simulating a transient network failure for retry testing.
  void SetDropFirstNConnections(int n) { dropFirstN_ = n; }

  // If set, every request missing a case-insensitively-matching header with
  // this exact value gets a 403 -- lets a test prove that custom headers
  // (OpenOptions::http_headers) actually reach every request, including the
  // ones SourceEngine::SeekAndDecode's fresh fallback reader makes.
  void SetRequiredHeader(std::string name, std::string value) {
    std::lock_guard<std::mutex> lock(headerMutex_);
    requiredHeaderName_ = std::move(name);
    requiredHeaderValue_ = std::move(value);
  }

  int RequestCount() const { return requestCount_; }

 private:
  void ServeLoop() {
    while (!stop_) {
      fd_set set;
      FD_ZERO(&set);
      FD_SET(listenFd_, &set);
      timeval tv{0, 100000};
      const int ready = select(static_cast<int>(listenFd_) + 1, &set, nullptr, nullptr, &tv);
      if (ready <= 0) continue;

      SocketHandle client = accept(listenFd_, nullptr, nullptr);
      if (client == kInvalidSocket) continue;

      if (dropFirstN_ > 0) {
        --dropFirstN_;
        CloseSocketHandle(client);
        continue;
      }
      ++requestCount_;
      HandleConnection(client);
      CloseSocketHandle(client);
    }
  }

  static void SendAll(SocketHandle client, const std::string& data) {
    size_t sent = 0;
    while (sent < data.size()) {
      // The client legitimately closes early (HttpByteSource aborts after
      // the headers of its probe request, or stops reading a range it no
      // longer needs). On Linux a send() to that closed socket raises
      // SIGPIPE, which killed the whole test binary intermittently
      // (timing-dependent); MSG_NOSIGNAL turns it into a plain error.
#if defined(MSG_NOSIGNAL)
      const int flags = MSG_NOSIGNAL;
#else
      const int flags = 0;
#endif
      const int n = send(client, data.data() + sent, static_cast<int>(data.size() - sent), flags);
      if (n <= 0) return;
      sent += static_cast<size_t>(n);
    }
  }

  void HandleConnection(SocketHandle client) {
    std::string request;
    char chunk[4096];
    // Read until the header terminator; this test server never expects a
    // request body (GET only).
    while (request.find("\r\n\r\n") == std::string::npos) {
      const int n = recv(client, chunk, sizeof(chunk), 0);
      if (n <= 0) return;
      request.append(chunk, static_cast<size_t>(n));
    }

    if (forceStatus_ != 0) {
      SendStatusOnly(client, forceStatus_);
      return;
    }

    std::map<std::string, std::string> headers;  // lowercased name -> value
    {
      std::istringstream stream(request);
      std::string line;
      std::getline(stream, line);  // request line, unused
      while (std::getline(stream, line)) {
        if (!line.empty() && line.back() == '\r') line.pop_back();
        if (line.empty()) break;
        const size_t colon = line.find(':');
        if (colon == std::string::npos) continue;
        std::string name = line.substr(0, colon);
        for (auto& c : name) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
        std::string value = line.substr(colon + 1);
        while (!value.empty() && value.front() == ' ') value.erase(value.begin());
        headers[name] = value;
      }
    }

    {
      std::lock_guard<std::mutex> lock(headerMutex_);
      if (!requiredHeaderName_.empty()) {
        std::string lowerName = requiredHeaderName_;
        for (auto& c : lowerName) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
        auto it = headers.find(lowerName);
        if (it == headers.end() || it->second != requiredHeaderValue_) {
          SendStatusOnly(client, 403);
          return;
        }
      }
    }

    std::string rangeHeader = headers.count("range") ? headers["range"] : std::string();
    if (rangeHeader.empty() || !rangeSupported_) {
      SendFullBody(client);
      return;
    }
    SendRangeResponse(client, rangeHeader);
  }

  void SendStatusOnly(SocketHandle client, int status) {
    std::ostringstream resp;
    resp << "HTTP/1.1 " << status << " " << ReasonPhrase(status) << "\r\n"
         << "Content-Length: 0\r\n"
         << "Connection: close\r\n\r\n";
    SendAll(client, resp.str());
  }

  void SendFullBody(SocketHandle client) {
    std::ostringstream resp;
    resp << "HTTP/1.1 200 OK\r\n"
         << "Content-Length: " << body_.size() << "\r\n"
         << "Connection: close\r\n\r\n";
    SendAll(client, resp.str());
    SendAll(client, std::string(reinterpret_cast<const char*>(body_.data()), body_.size()));
  }

  void SendRangeResponse(SocketHandle client, const std::string& rangeHeader) {
    // rangeHeader looks like "bytes=START-END" or "bytes=START-".
    const size_t eq = rangeHeader.find('=');
    const size_t dash = rangeHeader.find('-');
    if (eq == std::string::npos || dash == std::string::npos) {
      SendFullBody(client);
      return;
    }
    const uint64_t total = body_.size();
    const uint64_t start = std::strtoull(rangeHeader.c_str() + eq + 1, nullptr, 10);
    const std::string endStr = rangeHeader.substr(dash + 1);
    const uint64_t end = endStr.empty() ? (total > 0 ? total - 1 : 0) : std::strtoull(endStr.c_str(), nullptr, 10);

    if (start >= total || start > end) {
      std::ostringstream resp;
      resp << "HTTP/1.1 416 Range Not Satisfiable\r\n"
           << "Content-Range: bytes */" << total << "\r\n"
           << "Content-Length: 0\r\n"
           << "Connection: close\r\n\r\n";
      SendAll(client, resp.str());
      return;
    }
    const uint64_t clampedEnd = end >= total ? total - 1 : end;
    const uint64_t length = clampedEnd - start + 1;

    std::ostringstream resp;
    resp << "HTTP/1.1 206 Partial Content\r\n"
         << "Content-Range: bytes " << start << "-" << clampedEnd << "/" << total << "\r\n"
         << "Content-Length: " << length << "\r\n"
         << "Connection: close\r\n\r\n";
    SendAll(client, resp.str());
    SendAll(client, std::string(reinterpret_cast<const char*>(body_.data() + start), length));
  }

  static std::string ReasonPhrase(int status) {
    switch (status) {
      case 403:
        return "Forbidden";
      case 404:
        return "Not Found";
      default:
        return "Error";
    }
  }

  std::vector<uint8_t> body_;
  SocketHandle listenFd_ = kInvalidSocket;
  int port_ = 0;
  std::thread thread_;
  std::atomic<bool> stop_{false};
  std::atomic<bool> rangeSupported_{true};
  std::atomic<int> forceStatus_{0};
  std::atomic<int> dropFirstN_{0};
  std::atomic<int> requestCount_{0};
  std::mutex headerMutex_;
  std::string requiredHeaderName_;
  std::string requiredHeaderValue_;
};

}  // namespace mxfsrc_test
