// A loopback HTTP/1.1 origin for tests: one thread, one request per
// connection, answered by a handler the test supplies. It records every
// request it reads, so a test can prove what was sent, and that nothing was.
#ifndef GRPARSE_TESTS_SUPPORT_LOOPBACK_HTTP_H
#define GRPARSE_TESTS_SUPPORT_LOOPBACK_HTTP_H

#include <arpa/inet.h>
#include <netinet/in.h>
#include <poll.h>
#include <sys/socket.h>
#include <unistd.h>

#include <atomic>
#include <cctype>
#include <functional>
#include <mutex>
#include <string>
#include <string_view>
#include <thread>
#include <utility>
#include <vector>

#include "check.h"

namespace grparse_test {

class LoopbackHttp final {
 public:
  struct Request {
    std::string method;
    // The request target as sent: path and query.
    std::string target;
    // The header block, names lowercased, one "name: value" per entry.
    std::vector<std::pair<std::string, std::string>> headers;

    std::string header(std::string_view name) const {
      for (const auto& [key, value] : headers) {
        if (key == name) return value;
      }
      return std::string();
    }
  };

  // Builds a complete response with a Content-Length body.
  static std::string response(int status, const std::string& body,
                              const std::vector<std::pair<std::string, std::string>>& headers = {}) {
    std::string out = "HTTP/1.1 " + std::to_string(status) + " X\r\n";
    for (const auto& [name, value] : headers) out += name + ": " + value + "\r\n";
    out += "Content-Length: " + std::to_string(body.size()) + "\r\nConnection: close\r\n\r\n";
    return out + body;
  }

  explicit LoopbackHttp(std::function<std::string(const Request&)> handler)
      : handler_(std::move(handler)) {
    listener_ = ::socket(AF_INET, SOCK_STREAM, 0);
    require(listener_ >= 0, "loopback origin could not open a socket");
    const int reuse = 1;
    ::setsockopt(listener_, SOL_SOCKET, SO_REUSEADDR, &reuse, sizeof(reuse));
    sockaddr_in address{};
    address.sin_family = AF_INET;
    address.sin_addr.s_addr = ::htonl(INADDR_LOOPBACK);
    require(::bind(listener_, reinterpret_cast<sockaddr*>(&address), sizeof(address)) == 0,
            "loopback origin could not bind");
    require(::listen(listener_, 64) == 0, "loopback origin could not listen");
    socklen_t length = sizeof(address);
    require(::getsockname(listener_, reinterpret_cast<sockaddr*>(&address), &length) == 0,
            "loopback origin could not name its port");
    port_ = ::ntohs(address.sin_port);
    worker_ = std::thread([this] { serve(); });
  }

  ~LoopbackHttp() {
    stopping_.store(true);
    worker_.join();
    ::close(listener_);
  }

  LoopbackHttp(const LoopbackHttp&) = delete;
  LoopbackHttp& operator=(const LoopbackHttp&) = delete;

  // "http://127.0.0.1:<port>"; `host` swaps the host name (for example
  // "localhost", a second origin on the same socket).
  std::string base(const std::string& host = "127.0.0.1") const {
    return "http://" + host + ":" + std::to_string(port_);
  }

  std::vector<Request> received() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return received_;
  }

  // Connections accepted, whether or not a request arrived on them.
  int connections() const { return connections_.load(); }

 private:
  void serve() {
    while (!stopping_.load()) {
      pollfd waiting{listener_, POLLIN, 0};
      if (::poll(&waiting, 1, 50) <= 0) continue;
      const int connection = ::accept(listener_, nullptr, nullptr);
      if (connection < 0) continue;
      connections_.fetch_add(1);
      handle(connection);
      ::close(connection);
    }
  }

  void handle(int connection) {
    std::string buffer;
    size_t head_end = std::string::npos;
    while ((head_end = buffer.find("\r\n\r\n")) == std::string::npos) {
      pollfd readable{connection, POLLIN, 0};
      if (::poll(&readable, 1, 2000) <= 0) return;
      char chunk[4096];
      const ssize_t read = ::recv(connection, chunk, sizeof(chunk), 0);
      if (read <= 0) return;
      buffer.append(chunk, static_cast<size_t>(read));
    }
    Request request;
    const std::string head = buffer.substr(0, head_end);
    size_t line_end = head.find("\r\n");
    const std::string line = head.substr(0, line_end);
    const size_t method_end = line.find(' ');
    const size_t target_end = line.find(' ', method_end + 1);
    request.method = line.substr(0, method_end);
    request.target = line.substr(method_end + 1, target_end - method_end - 1);
    while (line_end != std::string::npos && line_end < head.size()) {
      const size_t start = line_end + 2;
      line_end = head.find("\r\n", start);
      const std::string header =
          head.substr(start, line_end == std::string::npos ? std::string::npos : line_end - start);
      const size_t colon = header.find(':');
      if (colon == std::string::npos) continue;
      std::string name = header.substr(0, colon);
      for (char& c : name) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
      size_t value = colon + 1;
      while (value < header.size() && header[value] == ' ') ++value;
      request.headers.emplace_back(name, header.substr(value));
    }
    const std::string answer = handler_(request);
    {
      std::lock_guard<std::mutex> lock(mutex_);
      received_.push_back(std::move(request));
    }
    ::send(connection, answer.data(), answer.size(), MSG_NOSIGNAL);
  }

  std::function<std::string(const Request&)> handler_;
  mutable std::mutex mutex_;
  std::vector<Request> received_;
  std::atomic<bool> stopping_{false};
  std::atomic<int> connections_{0};
  std::thread worker_;
  int listener_ = -1;
  int port_ = 0;
};

}  // namespace grparse_test

#endif
