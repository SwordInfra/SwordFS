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
  kSliceAppendReplyError,
  kSliceAppendProtocolFailureApplied,
  kSliceAppendDisconnectNotApplied,
  kSliceCounterExecProtocolFailureApplied,
  kSliceCounterExecDisconnectNotApplied,
  kSliceCounterExecAppliedThenFreshValue,
  kWriteExecReplyError,
  kWriteExecProtocolFailureApplied,
  kWriteExecDisconnectNotApplied,
  kReadOnlyExecProtocolFailure,
  kIncrProtocolFailureApplied,
  kIncrDisconnectNotApplied,
  kIncrProtocolFailureAppliedThenFreshValue,
  kOwnerSetReplyError,
  kOwnerRenewReplyError,
  kOwnerRevokeReplyError,
  kOwnerSetProtocolFailure,
  kOwnerRenewProtocolFailure,
  kOwnerRevokeProtocolFailure,
  kOwnerStalledReply,
  kOwnerLifecycleRenewError,
  kOwnerLifecycleRenewLostReply,
  kOwnerLifecycleRevokeError,
  kFormatQueueRejected,
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

  bool ReadExpectedIncrMutation(RespReader *reader) {
    std::vector<std::string> command;
    if (!reader->ReadCommand(&command)) {
      error_ = "failed to read RESP increment command";
      return false;
    }
    if (command.empty() || (command.front() != "INCR" && command.front() != "EVAL" && command.front() != "RPUSHX")) {
      error_ = "expected Redis INCR or EVAL command";
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

  void ServeOwnerCommand(std::string_view command, bool ambiguous_reply) {
    const int client = ::accept4(listen_fd_, nullptr, nullptr, SOCK_CLOEXEC);
    if (client < 0) {
      error_ = "accept failed";
      return;
    }
    connection_count_.fetch_add(1, std::memory_order_relaxed);
    RespReader reader(client);
    if (ReadExpected(&reader, command)) {
      // A broken acknowledgement cannot tell the client whether the
      // authoritative mutation ran. No automatic replay is safe here.
      mutation_applied_.store(ambiguous_reply, std::memory_order_relaxed);
      (void)Reply(client, ambiguous_reply ? "x\r\n" : "-ERR scripted failure\r\n");
    }
    ::close(client);
  }

  void ServeOwnerStalledReply() {
    const int client = ::accept4(listen_fd_, nullptr, nullptr, SOCK_CLOEXEC);
    if (client < 0) {
      error_ = "accept failed";
      return;
    }
    connection_count_.fetch_add(1, std::memory_order_relaxed);
    RespReader reader(client);
    std::vector<std::string> command;
    if (reader.ReadCommand(&command)) {
      // A server that receives a Redis command but never acknowledges it:
      // the per-command socket deadline, not server timing, ends the wait.
      std::this_thread::sleep_for(std::chrono::seconds(2));
    }
    ::close(client);
  }

  // Exercise the complete Redis-only owner lifecycle through the native
  // redis++ connection. Unlike standalone command faults, the owner must
  // first allocate a generation and successfully register before a renewal
  // or revoke reply can be faulted.
  void ServeOwnerLifecycle() {
    while (true) {
      const int client = ::accept4(listen_fd_, nullptr, nullptr, SOCK_CLOEXEC);
      if (client < 0) {
        break;  // Wait() closed the listening socket.
      }
      connection_count_.fetch_add(1, std::memory_order_relaxed);
      RespReader reader(client);
      std::vector<std::string> command;
      while (reader.ReadCommand(&command)) {
        if (command.empty()) {
          error_ = "empty owner lifecycle Redis command";
          break;
        }
        const auto &name = command.front();
        std::string_view reply;
        if (name == "GET") {
          reply = scenario_ == RedisFaultScenario::kFormatQueueRejected ? "$-1\r\n" : "$1\r\n0\r\n";
        } else if (name == "PING") {
          reply = "+PONG\r\n";
        } else if (name == "SET" && scenario_ == RedisFaultScenario::kFormatQueueRejected) {
          reply = "-ERR scripted queue rejection\r\n";
        } else if (name == "WATCH" || name == "MULTI" || name == "UNWATCH" || name == "DISCARD" || name == "SET") {
          reply = "+OK\r\n";
        } else if (name == "INCRBY") {
          reply = "+QUEUED\r\n";
        } else if (name == "EXEC") {
          exec_received_.store(true, std::memory_order_relaxed);
          reply = "*1\r\n:1\r\n";
        } else if (name == "PEXPIRE") {
          mutation_applied_.store(scenario_ == RedisFaultScenario::kOwnerLifecycleRenewLostReply,
                                  std::memory_order_relaxed);
          reply =
              scenario_ == RedisFaultScenario::kOwnerLifecycleRenewLostReply ? "x\r\n" : "-ERR renewal rejected\r\n";
        } else if (name == "DEL") {
          reply = scenario_ == RedisFaultScenario::kOwnerLifecycleRevokeError ? "-ERR revoke rejected\r\n" : ":1\r\n";
        } else {
          error_ = "unexpected owner lifecycle Redis command " + name;
          break;
        }
        if (!Reply(client, reply)) {
          break;
        }
      }
      ::close(client);
    }
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
    if (!ReadExpectedIncrMutation(&reader)) {
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

  void ServeSliceCounterExec(bool apply, bool protocol_failure) {
    const int client = ::accept4(listen_fd_, nullptr, nullptr, SOCK_CLOEXEC);
    if (client < 0) {
      error_ = "accept failed";
      return;
    }
    connection_count_.fetch_add(1, std::memory_order_relaxed);
    RespReader reader(client);
    std::vector<std::string> command;
    while (reader.ReadCommand(&command) && !command.empty()) {
      if (command.front() == "WATCH" || command.front() == "MULTI") {
        if (!Reply(client, "+OK\r\n")) {
          break;
        }
      } else if (command.front() == "GET") {
        if (!Reply(client, "$1\r\n0\r\n")) {
          break;
        }
      } else if (command.front() == "SET") {
        if (!Reply(client, "+QUEUED\r\n")) {
          break;
        }
      } else if (command.front() == "EXEC") {
        exec_received_.store(true, std::memory_order_relaxed);
        mutation_applied_.store(apply, std::memory_order_relaxed);
        if (protocol_failure) {
          (void)Reply(client, "x\r\n");
          ::close(client);
        } else {
          ResetAndClose(client);
        }
        return;
      } else {
        error_ = "unexpected SliceID allocation command " + command.front();
        break;
      }
    }
    ::close(client);
  }

  void ServeSliceCounterExecAppliedThenFreshValue() {
    // Both allocations use the Slice WATCH/GET/MULTI/SET/EXEC contract. The
    // first commit is durable but its acknowledgement is lost. A later caller
    // must see the advanced counter and allocate ID 2, never replay ID 1.
    uint64_t counter = 0;
    std::string counter_key;
    for (int attempt = 0; attempt < 2; ++attempt) {
      const int client = ::accept4(listen_fd_, nullptr, nullptr, SOCK_CLOEXEC);
      if (client < 0) {
        error_ = "accept failed";
        return;
      }
      connection_count_.fetch_add(1, std::memory_order_relaxed);
      RespReader reader(client);
      std::vector<std::string> command;
      bool completed = false;
      bool queued = false;
      uint64_t next_value = 0;
      while (reader.ReadCommand(&command) && !command.empty()) {
        if (command.front() == "WATCH" || command.front() == "MULTI") {
          if (!Reply(client, "+OK\r\n")) {
            break;
          }
        } else if (command.front() == "GET") {
          if (command.size() != 2 || (!counter_key.empty() && command[1] != counter_key)) {
            error_ = "Slice counter read used a different key";
            break;
          }
          counter_key = command[1];
          const auto raw = std::to_string(counter);
          if (!Reply(client, "$" + std::to_string(raw.size()) + "\r\n" + raw + "\r\n")) {
            break;
          }
        } else if (command.front() == "SET") {
          if (command.size() != 3 || command[1] != counter_key || queued) {
            error_ = "invalid Slice counter update";
            break;
          }
          const auto [end, parse_error] =
              std::from_chars(command[2].data(), command[2].data() + command[2].size(), next_value);
          if (parse_error != std::errc{} || end != command[2].data() + command[2].size() || next_value != counter + 1) {
            error_ = "Slice counter update did not advance the committed value";
            break;
          }
          queued = true;
          if (!Reply(client, "+QUEUED\r\n")) {
            break;
          }
        } else if (command.front() == "EXEC") {
          if (!queued) {
            error_ = "Slice counter transaction did not queue an update";
            break;
          }
          exec_received_.store(true, std::memory_order_relaxed);
          counter = next_value;
          mutation_applied_.store(true, std::memory_order_relaxed);
          (void)Reply(client, attempt == 0 ? "x\r\n" : "*1\r\n+OK\r\n");
          completed = true;
          break;
        } else {
          error_ = "unexpected Slice ChunkID allocation command " + command.front();
          break;
        }
      }
      ::close(client);
      if (!completed) {
        return;
      }
    }
  }

  void ServeIncrProtocolFailureAppliedThenFreshValue() {
    const int first_client = ::accept4(listen_fd_, nullptr, nullptr, SOCK_CLOEXEC);
    if (first_client < 0) {
      error_ = "accept failed";
      return;
    }
    connection_count_.fetch_add(1, std::memory_order_relaxed);
    RespReader first_reader(first_client);
    if (!ReadExpectedIncrMutation(&first_reader)) {
      ::close(first_client);
      return;
    }
    mutation_applied_.store(true, std::memory_order_relaxed);
    (void)Reply(first_client, "x\r\n");
    ::close(first_client);

    const int second_client = ::accept4(listen_fd_, nullptr, nullptr, SOCK_CLOEXEC);
    if (second_client < 0) {
      error_ = "accept failed";
      return;
    }
    connection_count_.fetch_add(1, std::memory_order_relaxed);
    RespReader second_reader(second_client);
    if (!ReadExpectedIncrMutation(&second_reader)) {
      ::close(second_client);
      return;
    }
    (void)Reply(second_client, ":2\r\n");
    ::close(second_client);
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
      case RedisFaultScenario::kSliceAppendReplyError:
        ServeStandaloneReplyError("RPUSHX");
        break;
      case RedisFaultScenario::kSliceAppendProtocolFailureApplied:
        ServeIncr(true, true);
        break;
      case RedisFaultScenario::kSliceAppendDisconnectNotApplied:
        ServeIncr(false, false);
        break;
      case RedisFaultScenario::kSliceCounterExecProtocolFailureApplied:
        ServeSliceCounterExec(true, true);
        break;
      case RedisFaultScenario::kSliceCounterExecDisconnectNotApplied:
        ServeSliceCounterExec(false, false);
        break;
      case RedisFaultScenario::kSliceCounterExecAppliedThenFreshValue:
        ServeSliceCounterExecAppliedThenFreshValue();
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
      case RedisFaultScenario::kIncrProtocolFailureAppliedThenFreshValue:
        ServeIncrProtocolFailureAppliedThenFreshValue();
        break;
      case RedisFaultScenario::kOwnerSetReplyError:
        ServeOwnerCommand("SET", false);
        break;
      case RedisFaultScenario::kOwnerRenewReplyError:
        ServeOwnerCommand("PEXPIRE", false);
        break;
      case RedisFaultScenario::kOwnerRevokeReplyError:
        ServeOwnerCommand("DEL", false);
        break;
      case RedisFaultScenario::kOwnerSetProtocolFailure:
        ServeOwnerCommand("SET", true);
        break;
      case RedisFaultScenario::kOwnerRenewProtocolFailure:
        ServeOwnerCommand("PEXPIRE", true);
        break;
      case RedisFaultScenario::kOwnerRevokeProtocolFailure:
        ServeOwnerCommand("DEL", true);
        break;
      case RedisFaultScenario::kOwnerStalledReply:
        ServeOwnerStalledReply();
        break;
      case RedisFaultScenario::kOwnerLifecycleRenewError:
      case RedisFaultScenario::kOwnerLifecycleRenewLostReply:
      case RedisFaultScenario::kOwnerLifecycleRevokeError:
      case RedisFaultScenario::kFormatQueueRejected:
        ServeOwnerLifecycle();
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
