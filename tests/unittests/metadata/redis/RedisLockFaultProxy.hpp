// Copyright 2026 SwordFS Contributors.
// Licensed under the Apache License, Version 2.0.

#pragma once

#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>

#include <algorithm>
#include <atomic>
#include <cerrno>
#include <charconv>
#include <cstdint>
#include <memory>
#include <mutex>
#include <stdexcept>
#include <string>
#include <string_view>
#include <thread>
#include <utility>
#include <vector>

namespace swordfs::metadata {

// Test-only RESP2 transport proxy backed by a real Redis server. It can
// discard an EXEC reply *after* the server committed or drop EXEC before
// delivery. No RedisMock or production test hook is required.
class RedisLockFaultProxy {
 public:
  enum class Fault { kAppliedLostReply, kNotAppliedLostReply };

  RedisLockFaultProxy(std::string host, int upstream_port, Fault fault, int lost_exec_replies)
      : host_(std::move(host)), upstream_port_(upstream_port), fault_(fault), remaining_(lost_exec_replies) {
    listener_ = ::socket(AF_INET, SOCK_STREAM | SOCK_CLOEXEC, 0);
    if (listener_ < 0) {
      throw std::runtime_error("failed to create Redis fault listener");
    }
    sockaddr_in address{};
    address.sin_family = AF_INET;
    address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    address.sin_port = 0;
    if (::bind(listener_, reinterpret_cast<sockaddr *>(&address), sizeof(address)) != 0 ||
        ::listen(listener_, 16) != 0) {
      ::close(listener_);
      throw std::runtime_error("failed to bind Redis fault listener");
    }
    socklen_t length = sizeof(address);
    if (::getsockname(listener_, reinterpret_cast<sockaddr *>(&address), &length) != 0) {
      ::close(listener_);
      throw std::runtime_error("failed to query Redis fault listener port");
    }
    port_ = ntohs(address.sin_port);
    accepting_ = std::thread([this] { Accept(); });
  }

  ~RedisLockFaultProxy() {
    (void)::shutdown(listener_, SHUT_RDWR);
    ::close(listener_);
    accepting_.join();
    {
      std::lock_guard lock(mutex_);
      for (const auto &session : sessions_) {
        if (session->client.load() >= 0) {
          (void)::shutdown(session->client.load(), SHUT_RDWR);
        }
      }
    }
    for (const auto &session : sessions_) {
      session->thread.join();
    }
  }

  RedisLockFaultProxy(const RedisLockFaultProxy &) = delete;
  RedisLockFaultProxy &operator=(const RedisLockFaultProxy &) = delete;

  int port() const {
    return port_;
  }

  int intercepted_execs() const {
    return intercepted_.load();
  }

 private:
  struct Session {
    explicit Session(int fd) : client(fd) {
    }
    std::atomic<int> client;
    std::thread thread;
  };

  static bool ReadExact(int fd, size_t n, std::string *out) {
    while (n > 0) {
      char bytes[4096];
      const auto result = ::recv(fd, bytes, std::min(n, sizeof(bytes)), 0);
      if (result <= 0) {
        return false;
      }
      out->append(bytes, static_cast<size_t>(result));
      n -= static_cast<size_t>(result);
    }
    return true;
  }

  static bool ReadLine(int fd, std::string *raw, std::string *line) {
    line->clear();
    while (true) {
      char character = 0;
      if (::recv(fd, &character, 1, 0) != 1) {
        return false;
      }
      raw->push_back(character);
      line->push_back(character);
      if (line->size() >= 2 && line->substr(line->size() - 2) == "\r\n") {
        line->resize(line->size() - 2);
        return true;
      }
      if (line->size() > 1024) {
        return false;
      }
    }
  }

  static bool Frame(int fd, std::string *raw) {
    char tag = 0;
    if (::recv(fd, &tag, 1, 0) != 1) {
      return false;
    }
    raw->push_back(tag);
    std::string line;
    if (!ReadLine(fd, raw, &line)) {
      return false;
    }
    if (tag == '+' || tag == '-' || tag == ':') {
      return true;
    }
    int64_t count = 0;
    const auto [end, error] = std::from_chars(line.data(), line.data() + line.size(), count);
    if (error != std::errc{} || end != line.data() + line.size() || count < -1 || count > (1 << 22)) {
      return false;
    }
    if (tag == '$') {
      return count == -1 || ReadExact(fd, static_cast<size_t>(count) + 2, raw);
    }
    if (tag != '*') {
      return false;
    }
    for (int64_t index = 0; index < count; ++index) {
      if (!Frame(fd, raw)) {
        return false;
      }
    }
    return true;
  }

  static bool Send(int fd, std::string_view bytes) {
    while (!bytes.empty()) {
      const auto sent = ::send(fd, bytes.data(), bytes.size(), MSG_NOSIGNAL);
      if (sent <= 0) {
        return false;
      }
      bytes.remove_prefix(static_cast<size_t>(sent));
    }
    return true;
  }

  static std::string Command(const std::string &raw) {
    const auto first = raw.find("\r\n$");
    if (first == std::string::npos) {
      return {};
    }
    const auto size_end = raw.find("\r\n", first + 3);
    if (size_end == std::string::npos) {
      return {};
    }
    return raw.substr(size_end + 2, raw.find("\r\n", size_end + 2) - size_end - 2);
  }

  void Relay(int fd, Session *session) {
    const int upstream = ::socket(AF_INET, SOCK_STREAM | SOCK_CLOEXEC, 0);
    if (upstream < 0) {
      return;
    }
    sockaddr_in address{};
    address.sin_family = AF_INET;
    address.sin_port = htons(static_cast<uint16_t>(upstream_port_));
    if (::inet_pton(AF_INET, host_.c_str(), &address.sin_addr) != 1 ||
        ::connect(upstream, reinterpret_cast<sockaddr *>(&address), sizeof(address)) != 0) {
      ::close(upstream);
      return;
    }
    while (true) {
      std::string request;
      if (!Frame(fd, &request)) {
        break;
      }
      const bool exec = Command(request) == "EXEC";
      bool fault = false;
      if (exec) {
        int left = remaining_.load();
        while (left > 0 && !remaining_.compare_exchange_weak(left, left - 1)) {
        }
        fault = left > 0;
      }
      if (fault && fault_ == Fault::kNotAppliedLostReply) {
        intercepted_.fetch_add(1);
        (void)Send(fd, "x\r\n");
        break;
      }
      if (!Send(upstream, request)) {
        break;
      }
      std::string response;
      if (!Frame(upstream, &response)) {
        break;
      }
      if (fault) {
        intercepted_.fetch_add(1);
        // Malformed reply is indistinguishable from a lost EXEC acknowledgement
        // to RedisMetaClient: the server actually committed, but the client
        // cannot determine that from the response.
        (void)Send(fd, "x\r\n");
        break;
      }
      if (!Send(fd, response)) {
        break;
      }
    }
    ::close(upstream);
    (void)::shutdown(fd, SHUT_RDWR);
    ::close(fd);
    session->client.store(-1);
  }

  void Accept() {
    while (true) {
      const int fd = ::accept4(listener_, nullptr, nullptr, SOCK_CLOEXEC);
      if (fd < 0) {
        break;
      }
      auto session = std::make_unique<Session>(fd);
      Session *ptr = session.get();
      session->thread = std::thread([this, ptr, fd] { Relay(fd, ptr); });
      std::lock_guard lock(mutex_);
      sessions_.push_back(std::move(session));
    }
  }

  const std::string host_;
  const int upstream_port_;
  const Fault fault_;
  std::atomic<int> remaining_;
  std::atomic<int> intercepted_{0};
  int listener_ = -1;
  int port_ = 0;
  std::thread accepting_;
  std::mutex mutex_;
  std::vector<std::unique_ptr<Session>> sessions_;
};

}  // namespace swordfs::metadata
