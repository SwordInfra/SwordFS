// Copyright 2026 SwordFS Contributors.
// Licensed under the Apache License, Version 2.0.

#pragma once

#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>

#include <array>
#include <atomic>
#include <cerrno>
#include <charconv>
#include <chrono>
#include <cstring>
#include <stdexcept>
#include <string>
#include <string_view>
#include <thread>
#include <utility>
#include <vector>

#include "metadata/redis/RedisMetaConfig.hpp"

namespace swordfs::metadata {

enum class RedisFaultScenario {
  kPreExecProtocolFailure,
  kReadOnlyPreExecProtocolFailure,
  kReadOnlyReplyError,
  kIncrReplyError,
  kWriteExecReplyError,
  kWriteExecProtocolFailureApplied,
  kWriteExecDisconnectNotApplied,
  kReadOnlyExecProtocolFailure,
  kIncrProtocolFailureApplied,
  kIncrDisconnectNotApplied,
};

class ScriptedRedisServer {
 public:
  explicit ScriptedRedisServer(RedisFaultScenario scenario, int expected_connections = 1)
      : scenario_(scenario), expected_connections_(expected_connections) {
    listen_fd_ = ::socket(AF_INET, SOCK_STREAM | SOCK_CLOEXEC, 0);
    if (listen_fd_ < 0) {
      throw std::runtime_error(std::string("socket: ") + std::strerror(errno));
    }

    const int reuse = 1;
    if (::setsockopt(listen_fd_, SOL_SOCKET, SO_REUSEADDR, &reuse, sizeof(reuse)) != 0) {
      throw std::runtime_error(std::string("setsockopt: ") + std::strerror(errno));
    }

    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    addr.sin_port = 0;
    if (::bind(listen_fd_, reinterpret_cast<sockaddr *>(&addr), sizeof(addr)) != 0) {
      throw std::runtime_error(std::string("bind: ") + std::strerror(errno));
    }
    if (::listen(listen_fd_, 8) != 0) {
      throw std::runtime_error(std::string("listen: ") + std::strerror(errno));
    }

    socklen_t len = sizeof(addr);
    if (::getsockname(listen_fd_, reinterpret_cast<sockaddr *>(&addr), &len) != 0) {
      throw std::runtime_error(std::string("getsockname: ") + std::strerror(errno));
    }
    port_ = ntohs(addr.sin_port);
    thread_ = std::thread([this] { Serve(); });
  }

  ~ScriptedRedisServer() {
    Wait();
  }

  ScriptedRedisServer(const ScriptedRedisServer &) = delete;
  ScriptedRedisServer &operator=(const ScriptedRedisServer &) = delete;

  uint16_t port() const {
    return port_;
  }

  void Wait() {
    if (listen_fd_ >= 0) {
      ::shutdown(listen_fd_, SHUT_RDWR);
      ::close(listen_fd_);
      listen_fd_ = -1;
    }
    if (thread_.joinable()) {
      thread_.join();
    }
  }

  int connection_count() const {
    return connection_count_.load(std::memory_order_relaxed);
  }

  bool exec_received() const {
    return exec_received_.load(std::memory_order_relaxed);
  }

  bool mutation_applied() const {
    return mutation_applied_.load(std::memory_order_relaxed);
  }

  const std::string &error() const {
    return error_;
  }

 private:
  class RespReader {
   public:
    explicit RespReader(int fd) : fd_(fd) {
    }

    bool ReadCommand(std::vector<std::string> *command) {
      std::string line;
      if (!ReadLine(&line) || line.empty() || line.front() != '*') {
        return false;
      }

      int count = 0;
      if (!ParseInteger(std::string_view(line).substr(1), &count) || count <= 0) {
        return false;
      }
      command->clear();
      command->reserve(static_cast<size_t>(count));
      for (int i = 0; i < count; ++i) {
        if (!ReadLine(&line) || line.empty() || line.front() != '$') {
          return false;
        }
        int size = 0;
        if (!ParseInteger(std::string_view(line).substr(1), &size) || size < 0) {
          return false;
        }
        std::string value;
        if (!ReadBytes(static_cast<size_t>(size), &value)) {
          return false;
        }
        std::string crlf;
        if (!ReadBytes(2, &crlf) || crlf != "\r\n") {
          return false;
        }
        command->push_back(std::move(value));
      }
      return true;
    }

   private:
    static bool ParseInteger(std::string_view value, int *out) {
      const auto [end, error] = std::from_chars(value.data(), value.data() + value.size(), *out);
      return error == std::errc{} && end == value.data() + value.size();
    }

    bool ReadLine(std::string *line) {
      while (true) {
        const auto end = buffer_.find("\r\n");
        if (end != std::string::npos) {
          *line = buffer_.substr(0, end);
          buffer_.erase(0, end + 2);
          return true;
        }
        if (!ReceiveMore()) {
          return false;
        }
      }
    }

    bool ReadBytes(size_t size, std::string *value) {
      while (buffer_.size() < size) {
        if (!ReceiveMore()) {
          return false;
        }
      }
      *value = buffer_.substr(0, size);
      buffer_.erase(0, size);
      return true;
    }

    bool ReceiveMore() {
      std::array<char, 1024> data{};
      const auto received = ::recv(fd_, data.data(), data.size(), 0);
      if (received <= 0) {
        return false;
      }
      buffer_.append(data.data(), static_cast<size_t>(received));
      return true;
    }

    int fd_;
    std::string buffer_;
  };

  static bool SendAll(int fd, std::string_view data) {
    while (!data.empty()) {
      const auto sent = ::send(fd, data.data(), data.size(), MSG_NOSIGNAL);
      if (sent <= 0) {
        return false;
      }
      data.remove_prefix(static_cast<size_t>(sent));
    }
    return true;
  }

  static void ResetAndClose(int fd) {
    linger reset{.l_onoff = 1, .l_linger = 0};
    (void)::setsockopt(fd, SOL_SOCKET, SO_LINGER, &reset, sizeof(reset));
    ::close(fd);
  }

  bool ReadExpected(RespReader *reader, std::string_view name) {
    std::vector<std::string> command;
    if (!reader->ReadCommand(&command)) {
      error_ = "failed to read RESP command " + std::string(name);
      return false;
    }
    if (command.empty() || command.front() != name) {
      error_ = "expected Redis command " + std::string(name);
      return false;
    }
    return true;
  }

  bool Reply(int fd, std::string_view reply) {
    if (!SendAll(fd, reply)) {
      error_ = "failed to send scripted Redis reply";
      return false;
    }
    return true;
  }

  void ServePreExecProtocolFailures() {
    for (int i = 0; i < expected_connections_; ++i) {
      const int client = ::accept4(listen_fd_, nullptr, nullptr, SOCK_CLOEXEC);
      if (client < 0) {
        error_ = "accept failed";
        return;
      }
      connection_count_.fetch_add(1, std::memory_order_relaxed);
      RespReader reader(client);
      if (!ReadExpected(&reader, "MULTI")) {
        ::close(client);
        return;
      }
      // This is intentionally not a valid RESP type byte. The failure happens
      // while opening MULTI, before EXEC can run any queued mutation.
      (void)Reply(client, "x\r\n");
      ::close(client);
    }
  }

  void ServeReadOnlyPreExecProtocolFailures() {
    for (int i = 0; i < expected_connections_; ++i) {
      const int client = ::accept4(listen_fd_, nullptr, nullptr, SOCK_CLOEXEC);
      if (client < 0) {
        error_ = "accept failed";
        return;
      }
      connection_count_.fetch_add(1, std::memory_order_relaxed);
      RespReader reader(client);
      if (!ReadExpected(&reader, "MULTI") || !Reply(client, "+OK\r\n")) {
        ::close(client);
        return;
      }
      if (!ReadExpected(&reader, "PING")) {
        ::close(client);
        return;
      }
      // PING is queued before EXEC. A malformed QUEUED acknowledgement must
      // remain safe to retry because no transaction command has executed.
      (void)Reply(client, "x\r\n");
      ::close(client);
    }
  }

  void ServeStandaloneReplyError(std::string_view command) {
    const int client = ::accept4(listen_fd_, nullptr, nullptr, SOCK_CLOEXEC);
    if (client < 0) {
      error_ = "accept failed";
      return;
    }
    connection_count_.fetch_add(1, std::memory_order_relaxed);
    RespReader reader(client);
    if (ReadExpected(&reader, command)) {
      (void)Reply(client, "-ERR scripted failure\r\n");
    }
    ::close(client);
  }

  void ServeWriteExecReplyError() {
    const int client = ::accept4(listen_fd_, nullptr, nullptr, SOCK_CLOEXEC);
    if (client < 0) {
      error_ = "accept failed";
      return;
    }
    connection_count_.fetch_add(1, std::memory_order_relaxed);
    RespReader reader(client);
    if (!ReadExpected(&reader, "MULTI") || !Reply(client, "+OK\r\n") || !ReadExpected(&reader, "SET") ||
        !Reply(client, "+QUEUED\r\n") || !ReadExpected(&reader, "EXEC")) {
      ::close(client);
      return;
    }
    exec_received_.store(true, std::memory_order_relaxed);
    (void)Reply(client, "-EXECABORT scripted failure\r\n");
    ::close(client);
  }

  void ServeTransaction(bool has_write, bool apply, bool protocol_failure) {
    const int client = ::accept4(listen_fd_, nullptr, nullptr, SOCK_CLOEXEC);
    if (client < 0) {
      error_ = "accept failed";
      return;
    }
    connection_count_.fetch_add(1, std::memory_order_relaxed);
    RespReader reader(client);
    if (!ReadExpected(&reader, "MULTI") || !Reply(client, "+OK\r\n")) {
      ::close(client);
      return;
    }
    if (!ReadExpected(&reader, has_write ? "SET" : "PING") || !Reply(client, "+QUEUED\r\n")) {
      ::close(client);
      return;
    }
    if (!ReadExpected(&reader, "EXEC")) {
      ::close(client);
      return;
    }
    exec_received_.store(true, std::memory_order_relaxed);
    mutation_applied_.store(apply, std::memory_order_relaxed);
    if (protocol_failure) {
      // The server-side transaction outcome has already been selected, but
      // the client receives bytes that cannot be parsed as a Redis reply.
      (void)Reply(client, "x\r\n");
      ::close(client);
    } else {
      ResetAndClose(client);
    }
  }

  void ServeIncr(bool apply, bool protocol_failure) {
    const int client = ::accept4(listen_fd_, nullptr, nullptr, SOCK_CLOEXEC);
    if (client < 0) {
      error_ = "accept failed";
      return;
    }
    connection_count_.fetch_add(1, std::memory_order_relaxed);
    RespReader reader(client);
    if (!ReadExpected(&reader, "INCR")) {
      ::close(client);
      return;
    }
    mutation_applied_.store(apply, std::memory_order_relaxed);
    if (protocol_failure) {
      (void)Reply(client, "x\r\n");
      ::close(client);
    } else {
      ResetAndClose(client);
    }
  }

  void Serve() {
    switch (scenario_) {
      case RedisFaultScenario::kPreExecProtocolFailure:
        ServePreExecProtocolFailures();
        break;
      case RedisFaultScenario::kReadOnlyPreExecProtocolFailure:
        ServeReadOnlyPreExecProtocolFailures();
        break;
      case RedisFaultScenario::kReadOnlyReplyError:
        ServeStandaloneReplyError("PING");
        break;
      case RedisFaultScenario::kIncrReplyError:
        ServeStandaloneReplyError("INCR");
        break;
      case RedisFaultScenario::kWriteExecReplyError:
        ServeWriteExecReplyError();
        break;
      case RedisFaultScenario::kWriteExecProtocolFailureApplied:
        ServeTransaction(true, true, true);
        break;
      case RedisFaultScenario::kWriteExecDisconnectNotApplied:
        ServeTransaction(true, false, false);
        break;
      case RedisFaultScenario::kReadOnlyExecProtocolFailure:
        ServeTransaction(false, false, true);
        break;
      case RedisFaultScenario::kIncrProtocolFailureApplied:
        ServeIncr(true, true);
        break;
      case RedisFaultScenario::kIncrDisconnectNotApplied:
        ServeIncr(false, false);
        break;
    }
  }

  RedisFaultScenario scenario_;
  int expected_connections_;
  int listen_fd_ = -1;
  uint16_t port_ = 0;
  std::thread thread_;
  std::atomic<int> connection_count_{0};
  std::atomic<bool> exec_received_{false};
  std::atomic<bool> mutation_applied_{false};
  std::string error_;
};

inline RedisMetaConfig ScriptedConfig(const ScriptedRedisServer &server) {
  RedisMetaConfig config;
  config.host = "127.0.0.1";
  config.port = server.port();
  config.connect_timeout = std::chrono::milliseconds(200);
  config.socket_timeout = std::chrono::milliseconds(200);
  config.pool_wait_timeout = std::chrono::milliseconds(200);
  config.retry_backoff = std::chrono::milliseconds(0);
  return config;
}

}  // namespace swordfs::metadata
