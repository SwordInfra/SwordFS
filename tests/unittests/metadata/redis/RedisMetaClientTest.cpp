// Copyright 2026 SwordFS Contributors.
// Licensed under the Apache License, Version 2.0.

#include <dirent.h>
#include <folly/Conv.h>
#include <folly/fibers/Baton.h>
#include <folly/fibers/FiberManagerMap.h>
#include <gtest/gtest.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>

#include <array>
#include <atomic>
#include <cerrno>
#include <charconv>
#include <chrono>
#include <cstdlib>
#include <cstring>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <thread>
#include <type_traits>
#include <utility>
#include <vector>

#include "chunk/ChunkObjectKey.hpp"
#include "chunk/IChunkOverwriteStrategy.hpp"
#include "chunk/WholeObjectCleanup.hpp"
#include "metadata/redis/RedisBackendContext.hpp"
#include "metadata/redis/RedisKey.hpp"
#include "metadata/redis/RedisKvTxn.hpp"
#include "metadata/redis/RedisMetaClient.hpp"
#include "metadata/redis/RedisMetaConfig.hpp"
#include "metadata/redis/RedisMetaOps.hpp"
#include "metadata/redis/RedisMetaTxn.hpp"
#include "metadata/redis/RedisTestUtils.hpp"
#include "metadata/types/Chunk.hpp"
#include "metadata/types/Inode.hpp"
#include "utils/ExecutionDomain.hpp"

namespace swordfs::metadata {
namespace {

constexpr SetAttrField kKillSuidGidField = SetAttrField::kKillSuidGid;

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

RedisMetaConfig ScriptedConfig(const ScriptedRedisServer &server) {
  RedisMetaConfig config;
  config.host = "127.0.0.1";
  config.port = server.port();
  config.connect_timeout = std::chrono::milliseconds(200);
  config.socket_timeout = std::chrono::milliseconds(200);
  config.pool_wait_timeout = std::chrono::milliseconds(200);
  config.retry_backoff = std::chrono::milliseconds(0);
  return config;
}

template <typename Fn>
auto RunInFiber(Fn &&fn) -> decltype(fn()) {
  using Result = decltype(fn());
  folly::EventBase evb;
  auto &manager = folly::fibers::getFiberManager(evb);
  folly::fibers::Baton done;
  if constexpr (std::is_void_v<Result>) {
    manager.addTask([&] {
      fn();
      done.post();
    });
    while (!done.try_wait()) {
      evb.loopOnce();
    }
    return;
  } else {
    std::optional<Result> result;
    manager.addTask([&] {
      result = fn();
      done.post();
    });
    while (!done.try_wait()) {
      evb.loopOnce();
    }
    return std::move(*result);
  }
}

const char *RedisTestUrl() {
  return std::getenv("SWORDFS_REDIS_TEST_URL");
}

bool ParseTestConfig(RedisMetaConfig *config) {
  const char *url = RedisTestUrl();
  if (url == nullptr) {
    return false;
  }
  const auto status = ParseRedisMetaUrl(url, config);
  EXPECT_TRUE(status.ok()) << status.message();
  return status.ok();
}

sw::redis::ConnectionOptions ConnectionOptions(const RedisMetaConfig &config) {
  sw::redis::ConnectionOptions options;
  options.host = config.host;
  options.port = config.port;
  options.db = config.db;
  return options;
}

std::string UniqueRedisName(std::string_view suffix) {
  return swordfs::test::UniqueRedisTestNamespace("redis-meta-txn", suffix);
}

uint64_t RedisInfoCounter(sw::redis::Redis &redis, std::string_view section, std::string_view name) {
  const auto info = redis.info(section);
  const auto prefix = std::string(name) + ":";
  const auto begin = info.find(prefix) + prefix.size();
  const auto end = info.find('\r', begin);
  return folly::to<uint64_t>(std::string_view(info).substr(begin, end - begin));
}

utils::Status SeedInode(sw::redis::Redis &redis, const redis::RedisKey &key, const SwordFsInode &inode) {
  std::string value;
  auto status = inode.SerializeTo(&value);
  if (!status.ok()) {
    return status;
  }
  redis.set(key.Inode(inode.ino), value);
  return utils::Status::OK();
}

utils::Status SeedEntry(sw::redis::Redis &redis, const redis::RedisKey &key, InodeID parent_ino,
                        const SwordFsEntry &entry) {
  std::string value;
  auto status = entry.SerializeTo(&value);
  if (!status.ok()) {
    return status;
  }
  redis.hset(key.Directory(parent_ino), entry.name, value);
  return utils::Status::OK();
}

utils::Status CommitChunkTxn(RedisMetaClient &store, const redis::RedisKey &key, uint64_t chunk_size, InodeID ino,
                             const std::optional<SwordFsChunk> &expected, const SwordFsChunk &replacement) {
  utils::Status publication_result;
  std::optional<PendingDelete> cleanup_candidate;
  auto status = store.Transact([&](RedisKvTxn &kv_txn) {
    RedisMetaTxn txn(kv_txn, key, chunk_size);
    return txn.CommitChunk(ino, expected, replacement, publication_result, cleanup_candidate);
  });
  if (!status.ok()) {
    return status;
  }
  return publication_result;
}

class RecordingRedisIndex final : public IChunkIndexParticipant {
 public:
  bool reject_publish = false;

  utils::Status LoadPublished(IChunkIndexReader &reader, InodeID file_ino, const SwordFsChunk &head,
                              std::string *private_snapshot) const override {
    return reader.Read("fragments:" + std::to_string(file_ino), std::to_string(head.revision), private_snapshot);
  }
  utils::Status Publish(IChunkIndexTxn &txn, InodeID file_ino, const std::optional<SwordFsChunk> &,
                        const SwordFsChunk &replacement, const ChunkPublishIntent &intent) const override {
    // This test record is auxiliary; a real slice manifest needed for reads
    // must be durable before the public head is queued in Redis EXEC.
    const auto value = intent.payload.empty() ? "staged" : intent.payload;
    auto status = txn.Put("fragments:" + std::to_string(file_ino), std::to_string(replacement.revision), value);
    if (!status.ok()) {
      return status;
    }
    return reject_publish ? utils::Status::IOError("reject private publication") : utils::Status::OK();
  }
  utils::Status Truncate(IChunkIndexTxn &, InodeID, const std::vector<ChunkIndexChange> &) const override {
    return utils::Status::OK();
  }
  utils::Status PrepareReclaim(IChunkIndexTxn &, InodeID, const std::vector<SwordFsChunk> &) const override {
    return utils::Status::OK();
  }
};

class RecordingRedisStrategy final : public chunk::IChunkOverwriteStrategy {
 public:
  RecordingRedisIndex index;

  metadata::ChunkOverwriteMechanism mechanism() const override {
    return metadata::ChunkOverwriteMechanism::kRedisCache;
  }
  uint32_t index_format_version() const override {
    return 1;
  }
  std::shared_ptr<chunk::IChunkSession> OpenSession(InodeID file_ino, ChunkIndex chunk_index) const override {
    return chunk::DefaultChunkOverwriteStrategy().OpenSession(file_ino, chunk_index);
  }
  const IChunkIndexParticipant &index_participant() const override {
    return index;
  }
  utils::Status FreezePendingDelete(IChunkIndexTxn &, InodeID file_ino, const SwordFsChunk &head, uint64_t chunk_size,
                                    PendingDelete *out) const override {
    return chunk::FreezeWholeObjectDelete(file_ino, head, chunk_size, out);
  }
  utils::Status FreezeRejectedPublication(InodeID file_ino, const SwordFsChunk &replacement, const ChunkPublishIntent &,
                                          uint64_t chunk_size, PendingDelete *out) const override {
    return chunk::FreezeWholeObjectDelete(file_ino, replacement, chunk_size, out);
  }
  utils::Status FreezeReclaim(IChunkIndexTxn &, InodeID file_ino, const std::vector<SwordFsChunk> &heads,
                              uint64_t chunk_size, ReclaimWork *out) const override {
    return chunk::FreezeWholeObjectReclaim(file_ino, heads, chunk_size, out);
  }
  utils::Status DeletePending(const PendingDelete &work, uint64_t chunk_size, IMetaEngine *meta,
                              storage::IDataEngine *data, bool *completed) const override {
    return chunk::DefaultChunkOverwriteStrategy().DeletePending(work, chunk_size, meta, data, completed);
  }
  utils::Status DeleteFrozen(const ReclaimWork &work, uint64_t chunk_size, IMetaEngine *meta,
                             storage::IDataEngine *data) const override {
    return chunk::DefaultChunkOverwriteStrategy().DeleteFrozen(work, chunk_size, meta, data);
  }
};

}  // namespace

TEST(RedisMetaClientTest, BinaryValueRoundTrip) {
  RedisMetaConfig config;
  if (!ParseTestConfig(&config)) {
    GTEST_SKIP() << "SWORDFS_REDIS_TEST_URL is not configured";
  }

  RedisMetaClient store(config);
  const std::string key = UniqueRedisName("binary-roundtrip");
  const std::string value(172, '\0');
  sw::redis::Redis cleanup(ConnectionOptions(config));
  cleanup.set(key, value);

  std::string actual;
  auto status = store.Get(key, &actual);
  ASSERT_TRUE(status.ok()) << status.message();
  EXPECT_EQ(actual.size(), value.size());
  EXPECT_EQ(actual, value);
  cleanup.del(key);
}

TEST(RedisMetaClientTest, BinaryValueSurvivesWriteTransaction) {
  RedisMetaConfig config;
  if (!ParseTestConfig(&config)) {
    GTEST_SKIP() << "SWORDFS_REDIS_TEST_URL is not configured";
  }

  RedisMetaClient store(config);
  const std::string parent_key = UniqueRedisName("txn-parent");
  const std::string child_key = UniqueRedisName("txn-child");
  const std::string child_value(172, '\0');
  std::atomic<bool> callback_in_thread_domain{false};

  auto status = store.Transact([&](RedisKvTxn &txn) {
    callback_in_thread_domain.store(utils::CurrentExecutionDomain() == utils::ExecutionDomain::kThread,
                                    std::memory_order_relaxed);
    std::string ignored;
    auto status = txn.Get(parent_key, &ignored);
    if (!status.IsNotFound()) {
      return status;
    }
    status = txn.Set(child_key, child_value);
    if (!status.ok()) {
      return status;
    }
    return txn.Set(parent_key, "parent");
  });
  ASSERT_TRUE(status.ok()) << status.message();
  EXPECT_TRUE(callback_in_thread_domain.load(std::memory_order_relaxed));

  std::string actual;
  status = store.Get(child_key, &actual);
  ASSERT_TRUE(status.ok()) << status.message();
  EXPECT_EQ(actual.size(), child_value.size());
  EXPECT_EQ(actual, child_value);

  sw::redis::Redis cleanup(ConnectionOptions(config));
  cleanup.del(parent_key);
  cleanup.del(child_key);
}

TEST(RedisMetaClientTest, DetectsExecCommandErrorAndReportsPossiblePartialCommit) {
  RedisMetaConfig config;
  if (!ParseTestConfig(&config)) {
    GTEST_SKIP() << "SWORDFS_REDIS_TEST_URL is not configured";
  }

  RedisMetaClient store(config);
  sw::redis::Redis redis(ConnectionOptions(config));
  const std::string wrong_type_key = UniqueRedisName("exec-wrongtype");
  const std::string later_key = UniqueRedisName("exec-later-write");
  redis.del(wrong_type_key);
  redis.del(later_key);
  redis.set(wrong_type_key, "string-value");

  const auto status = store.Transact([&](RedisKvTxn &txn) {
    auto txn_status = txn.HSet(wrong_type_key, "field", "value");
    if (!txn_status.ok()) {
      return txn_status;
    }
    return txn.Set(later_key, "applied");
  });

  EXPECT_TRUE(status.IsOutcomeUnknown());
  EXPECT_EQ(status.ToErrno(), EIO);
  EXPECT_NE(status.message().find("commit may be partial"), std::string::npos);
  const auto later_value = redis.get(later_key);
  ASSERT_TRUE(later_value.has_value());
  EXPECT_EQ(*later_value, "applied");

  redis.del(wrong_type_key);
  redis.del(later_key);
}

TEST(RedisMetaClientTest, StandalonePingAndWatchReadMultiExec) {
  RedisMetaConfig config;
  if (!ParseTestConfig(&config)) {
    GTEST_SKIP() << "SWORDFS_REDIS_TEST_URL is not configured";
  }

  RedisMetaClient store(config);
  auto status = store.Ping();
  ASSERT_TRUE(status.ok()) << status.message();

  const std::string key = UniqueRedisName("watch-read-write");
  sw::redis::Redis cleanup(ConnectionOptions(config));
  cleanup.set(key, "before");

  status = store.Transact([&](RedisKvTxn &transaction) {
    std::string value;
    auto txn_status = transaction.Get(key, &value);
    if (!txn_status.ok()) {
      return txn_status;
    }
    EXPECT_EQ(value, "before");
    return transaction.Set(key, "ok");
  });
  ASSERT_TRUE(status.ok()) << status.message();

  status = store.Transact([&](RedisKvTxn &transaction) {
    std::string value;
    auto txn_status = transaction.Get(key, &value);
    if (!txn_status.ok()) {
      return txn_status;
    }
    if (value != "ok") {
      return utils::Status::IOError("unexpected Redis value");
    }
    return utils::Status::OK();
  });
  EXPECT_TRUE(status.ok()) << status.message();
  cleanup.del(key);
}

TEST(RedisMetaClientTest, SuccessfulTransactionsReusePooledConnection) {
  RedisMetaConfig config;
  if (!ParseTestConfig(&config)) {
    GTEST_SKIP() << "SWORDFS_REDIS_TEST_URL is not configured";
  }

  // A single-slot pool makes connection reuse observable without depending on
  // scheduler interleavings. The independent observer keeps one connection
  // open while reading Redis' cumulative accepted-connection counter.
  config.pool_size = 1;
  RedisMetaClient store(config);
  sw::redis::Redis observer(ConnectionOptions(config));
  const std::string key = UniqueRedisName("pooled-connection-reuse");
  observer.set(key, "0");

  auto transact_once = [&] {
    return store.Transact([&](RedisKvTxn &transaction) {
      std::string value;
      const auto status = transaction.Get(key, &value);
      EXPECT_TRUE(status.ok()) << status.message();
      return transaction.Set(key, value == "0" ? "1" : "0");
    });
  };

  // Warm the lazy redis++ pool before taking the baseline so normal initial
  // connection establishment is excluded from the measured delta.
  auto status = transact_once();
  ASSERT_TRUE(status.ok()) << status.message();
  const auto before = RedisInfoCounter(observer, "stats", "total_connections_received");

  constexpr int kTransactionCount = 64;
  for (int i = 0; i < kTransactionCount; ++i) {
    status = transact_once();
    ASSERT_TRUE(status.ok()) << "transaction " << i << ": " << status.message();
  }

  const auto after = RedisInfoCounter(observer, "stats", "total_connections_received");
  ASSERT_GE(after, before);
  const auto connection_delta = after - before;

  // The compose Redis healthcheck can contribute a small amount of background
  // traffic, so assert a generous bounded delta rather than an exact value.
  // A healthy one-slot pool reuses its connection; reconnect-per-transaction
  // behavior grows approximately with kTransactionCount and must fail here.
  EXPECT_LE(connection_delta, 16U) << "64 successful transactions opened " << connection_delta
                                   << " additional Redis connections";

  observer.del(key);
}

TEST(RedisMetaClientTest, RetriesWatchConflict) {
  RedisMetaConfig config;
  if (!ParseTestConfig(&config)) {
    GTEST_SKIP() << "SWORDFS_REDIS_TEST_URL is not configured";
  }

  const std::string key = UniqueRedisName("watch-conflict");
  sw::redis::Redis other(ConnectionOptions(config));
  other.set(key, "before");

  RedisMetaClient store(config);
  int attempts = 0;
  const auto status = store.Transact([&](RedisKvTxn &transaction) {
    ++attempts;
    std::string value;
    auto txn_status = transaction.Get(key, &value);
    if (!txn_status.ok()) {
      return txn_status;
    }

    if (attempts == 1) {
      other.set(key, "raced");
    }
    return transaction.Set(key, "committed");
  });

  ASSERT_TRUE(status.ok()) << status.message();
  EXPECT_EQ(attempts, 2);
  const auto committed = other.get(key);
  ASSERT_TRUE(committed.has_value());
  EXPECT_EQ(*committed, "committed");
  other.del(key);
}

TEST(RedisMetaClientTest, RetriesReadOnlyWatchConflict) {
  RedisMetaConfig config;
  if (!ParseTestConfig(&config)) {
    GTEST_SKIP() << "SWORDFS_REDIS_TEST_URL is not configured";
  }

  const std::string key = UniqueRedisName("read-only-watch-conflict");
  sw::redis::Redis other(ConnectionOptions(config));
  other.set(key, "before");

  RedisMetaClient store(config);
  int attempts = 0;
  const auto status = store.Transact([&](RedisKvTxn &transaction) {
    ++attempts;
    std::string value;
    auto txn_status = transaction.Get(key, &value);
    if (!txn_status.ok()) {
      return txn_status;
    }

    if (attempts == 1) {
      other.set(key, "raced");
    }
    return utils::Status::OK();
  });

  ASSERT_TRUE(status.ok()) << status.message();
  EXPECT_EQ(attempts, 2);
  other.del(key);
}

TEST(RedisMetaClientTest, ReturnsCallbackBusyWithoutRetry) {
  RedisMetaConfig config;
  if (!ParseTestConfig(&config)) {
    GTEST_SKIP() << "SWORDFS_REDIS_TEST_URL is not configured";
  }

  RedisMetaClient store(config);
  int attempts = 0;
  const auto status = store.Transact([&](RedisKvTxn &) {
    ++attempts;
    return utils::Status::Busy("filesystem operation is busy");
  });
  EXPECT_TRUE(status.ToErrno() == EBUSY);
  EXPECT_EQ(status.message(), "filesystem operation is busy");
  EXPECT_EQ(attempts, 1);
}

TEST(RedisMetaClientTest, DoesNotRetryWatchErrorEscapingFromCallback) {
  RedisMetaConfig config;
  if (!ParseTestConfig(&config)) {
    GTEST_SKIP() << "SWORDFS_REDIS_TEST_URL is not configured";
  }

  RedisMetaClient store(config);
  int attempts = 0;
  const auto status = store.Transact([&](RedisKvTxn &) -> utils::Status {
    ++attempts;
    throw sw::redis::WatchError();
  });

  EXPECT_EQ(status.ToErrno(), EIO);
  EXPECT_EQ(attempts, 1);
}

TEST(RedisMetaClientTest, RejectsNonPositiveRetryAttemptsDuringConstruction) {
  RedisMetaConfig config;
  config.host = "127.0.0.1";
  for (const int retry_attempts : {0, -1}) {
    config.retry_attempts = retry_attempts;
    EXPECT_THROW({ RedisMetaClient store(config); }, std::invalid_argument);
  }
}

TEST(RedisMetaClientTest, RejectsConfigurationThatCanCreateUnboundedRedisWaits) {
  RedisMetaConfig config;
  config.host = "127.0.0.1";

  auto invalid = config;
  invalid.connect_timeout = std::chrono::milliseconds(0);
  EXPECT_THROW({ RedisMetaClient client(invalid); }, std::invalid_argument);

  invalid = config;
  invalid.socket_timeout = std::chrono::milliseconds(0);
  EXPECT_THROW({ RedisMetaClient client(invalid); }, std::invalid_argument);

  invalid = config;
  invalid.pool_wait_timeout = std::chrono::milliseconds(0);
  EXPECT_THROW({ RedisMetaClient client(invalid); }, std::invalid_argument);

  invalid = config;
  invalid.pool_size = 0;
  EXPECT_THROW({ RedisMetaClient client(invalid); }, std::invalid_argument);

  invalid = config;
  invalid.retry_backoff = std::chrono::milliseconds(-1);
  EXPECT_THROW({ RedisMetaClient client(invalid); }, std::invalid_argument);
}

TEST(RedisMetaClientTest, WatchConflictRetryLimitReturnsUnavailable) {
  RedisMetaConfig config;
  if (!ParseTestConfig(&config)) {
    GTEST_SKIP() << "SWORDFS_REDIS_TEST_URL is not configured";
  }
  config.retry_attempts = 3;
  config.retry_backoff = std::chrono::milliseconds(0);

  const std::string key = UniqueRedisName("watch-retry-limit");
  sw::redis::Redis other(ConnectionOptions(config));
  other.set(key, "0");

  RedisMetaClient store(config);
  int attempts = 0;
  const auto status = store.Transact([&](RedisKvTxn &transaction) {
    ++attempts;
    std::string value;
    auto status = transaction.Get(key, &value);
    if (!status.ok()) {
      return status;
    }
    other.incr(key);
    return transaction.Set(key, "committed");
  });

  EXPECT_TRUE(status.IsUnavailable());
  EXPECT_EQ(status.ToErrno(), EIO);
  EXPECT_EQ(status.message(), "Redis transaction retry limit exceeded");
  EXPECT_EQ(attempts, 3);
  other.del(key);
}

TEST(RedisMetaClientTest, RetriesPreExecProtocolFailureAndReturnsUnavailable) {
  ScriptedRedisServer server(RedisFaultScenario::kPreExecProtocolFailure, 3);
  auto config = ScriptedConfig(server);
  config.retry_attempts = 3;

  RedisMetaClient store(config);
  int attempts = 0;
  const auto status = store.Transact([&](RedisKvTxn &transaction) {
    ++attempts;
    return transaction.Set("key", "value");
  });
  server.Wait();

  EXPECT_TRUE(status.IsUnavailable()) << status.message();
  EXPECT_EQ(attempts, 3);
  EXPECT_EQ(server.connection_count(), 3);
  EXPECT_TRUE(server.error().empty()) << server.error();
}

TEST(RedisMetaClientTest, RetriesReadOnlyPreExecProtocolFailureAndReturnsUnavailable) {
  ScriptedRedisServer server(RedisFaultScenario::kReadOnlyPreExecProtocolFailure, 3);
  auto config = ScriptedConfig(server);
  config.retry_attempts = 3;

  RedisMetaClient store(config);
  const auto status = store.Transact([](RedisKvTxn &) { return utils::Status::OK(); });
  server.Wait();

  EXPECT_TRUE(status.IsUnavailable()) << status.message();
  EXPECT_EQ(server.connection_count(), 3);
  EXPECT_FALSE(server.exec_received());
  EXPECT_TRUE(server.error().empty()) << server.error();
}

TEST(RedisMetaClientTest, ValidRedisReplyErrorsRemainKnownFailuresWithoutRetry) {
  {
    ScriptedRedisServer server(RedisFaultScenario::kReadOnlyReplyError);
    RedisMetaClient store(ScriptedConfig(server));
    const auto status = store.Ping();
    server.Wait();

    EXPECT_EQ(status.ToErrno(), EIO);
    EXPECT_FALSE(status.IsUnavailable());
    EXPECT_FALSE(status.IsOutcomeUnknown());
    EXPECT_EQ(server.connection_count(), 1);
    EXPECT_TRUE(server.error().empty()) << server.error();
  }

  {
    ScriptedRedisServer server(RedisFaultScenario::kWriteExecReplyError);
    auto config = ScriptedConfig(server);
    config.retry_attempts = 3;
    RedisMetaClient store(config);

    int attempts = 0;
    const auto status = store.Transact([&](RedisKvTxn &transaction) {
      ++attempts;
      return transaction.Set("key", "value");
    });
    server.Wait();

    EXPECT_EQ(status.ToErrno(), EIO);
    EXPECT_FALSE(status.IsUnavailable());
    EXPECT_FALSE(status.IsOutcomeUnknown());
    EXPECT_EQ(attempts, 1);
    EXPECT_TRUE(server.exec_received());
    EXPECT_TRUE(server.error().empty()) << server.error();
  }
}

TEST(RedisMetaClientTest, PostExecProtocolFailureIsOutcomeUnknownWithoutReplay) {
  ScriptedRedisServer server(RedisFaultScenario::kWriteExecProtocolFailureApplied);
  auto config = ScriptedConfig(server);
  config.retry_attempts = 3;

  RedisMetaClient store(config);
  int attempts = 0;
  const auto status = store.Transact([&](RedisKvTxn &transaction) {
    ++attempts;
    return transaction.Set("key", "value");
  });
  server.Wait();

  EXPECT_TRUE(status.IsOutcomeUnknown()) << status.message();
  EXPECT_EQ(attempts, 1);
  EXPECT_TRUE(server.exec_received());
  EXPECT_TRUE(server.mutation_applied());
  EXPECT_TRUE(server.error().empty()) << server.error();
}

TEST(RedisMetaClientTest, PostExecDisconnectCanBeNotAppliedAndStillOutcomeUnknown) {
  ScriptedRedisServer server(RedisFaultScenario::kWriteExecDisconnectNotApplied);
  auto config = ScriptedConfig(server);
  config.retry_attempts = 3;

  RedisMetaClient store(config);
  int attempts = 0;
  const auto status = store.Transact([&](RedisKvTxn &transaction) {
    ++attempts;
    return transaction.Set("key", "value");
  });
  server.Wait();

  EXPECT_TRUE(status.IsOutcomeUnknown()) << status.message();
  EXPECT_EQ(attempts, 1);
  EXPECT_TRUE(server.exec_received());
  EXPECT_FALSE(server.mutation_applied());
  EXPECT_TRUE(server.error().empty()) << server.error();
}

TEST(RedisMetaClientTest, ReadOnlyExecProtocolFailureReturnsUnavailable) {
  ScriptedRedisServer server(RedisFaultScenario::kReadOnlyExecProtocolFailure);
  auto config = ScriptedConfig(server);

  RedisMetaClient store(config);
  const auto status = store.Transact([](RedisKvTxn &) { return utils::Status::OK(); });
  server.Wait();

  EXPECT_TRUE(status.IsUnavailable()) << status.message();
  EXPECT_TRUE(server.exec_received());
  EXPECT_FALSE(server.mutation_applied());
  EXPECT_TRUE(server.error().empty()) << server.error();
}

TEST(RedisMetaClientTest, DirectIncrReplyErrorIsKnownFailure) {
  ScriptedRedisServer server(RedisFaultScenario::kIncrReplyError);
  auto config = ScriptedConfig(server);
  RedisMetaClient store(config);

  uint64_t value = 0;
  const auto status = store.Incr("counter", &value);
  server.Wait();

  EXPECT_EQ(status.ToErrno(), EIO);
  EXPECT_FALSE(status.IsUnavailable());
  EXPECT_FALSE(status.IsOutcomeUnknown());
  EXPECT_TRUE(server.error().empty()) << server.error();
}

TEST(RedisMetaClientTest, DirectIncrLostAcknowledgementIsOutcomeUnknownForBothDurableOutcomes) {
  for (const auto scenario :
       {RedisFaultScenario::kIncrProtocolFailureApplied, RedisFaultScenario::kIncrDisconnectNotApplied}) {
    ScriptedRedisServer server(scenario);
    auto config = ScriptedConfig(server);
    RedisMetaClient store(config);

    uint64_t value = 0;
    const auto status = store.Incr("counter", &value);
    server.Wait();

    EXPECT_TRUE(status.IsOutcomeUnknown()) << status.message();
    EXPECT_TRUE(server.error().empty()) << server.error();
    if (scenario == RedisFaultScenario::kIncrProtocolFailureApplied) {
      EXPECT_TRUE(server.mutation_applied());
    } else {
      EXPECT_FALSE(server.mutation_applied());
    }
  }
}

TEST(RedisMetaClientTest, DoesNotRetryAmbiguousExecTimeout) {
  RedisMetaConfig config;
  if (!ParseTestConfig(&config)) {
    GTEST_SKIP() << "SWORDFS_REDIS_TEST_URL is not configured";
  }

  sw::redis::Redis control(ConnectionOptions(config));
  const std::string key = UniqueRedisName("ambiguous-exec-timeout");
  control.del(key);

  config.socket_timeout = std::chrono::milliseconds(50);
  config.retry_attempts = 3;
  config.retry_backoff = std::chrono::milliseconds(0);
  RedisMetaClient store(config);
  int attempts = 0;
  const auto status = store.Transact([&](RedisKvTxn &transaction) {
    ++attempts;
    auto status = transaction.Set(key, "possibly-committed");
    if (!status.ok()) {
      return status;
    }

    // Pause after the write has been queued in Redis but before EXEC. The
    // client times out waiting for EXEC, whose commit result is therefore
    // ambiguous and must never be replayed.
    control.command<void>("CLIENT", "PAUSE", 1000, "ALL");
    return utils::Status::OK();
  });
  control.command<void>("CLIENT", "UNPAUSE");

  EXPECT_TRUE(status.IsOutcomeUnknown());
  EXPECT_EQ(status.ToErrno(), EIO);
  EXPECT_NE(status.message().find("ambiguous after EXEC"), std::string::npos);
  EXPECT_EQ(attempts, 1);
  control.del(key);
}

TEST(RedisMetaClientTest, DoesNotRetryAmbiguousExecConnectionClose) {
  RedisMetaConfig config;
  if (!ParseTestConfig(&config)) {
    GTEST_SKIP() << "SWORDFS_REDIS_TEST_URL is not configured";
  }

  sw::redis::Redis control(ConnectionOptions(config));
  const std::string key = UniqueRedisName("ambiguous-exec-close");
  control.del(key);

  config.retry_attempts = 3;
  config.retry_backoff = std::chrono::milliseconds(0);
  RedisMetaClient store(config);
  int attempts = 0;
  const auto status = store.Transact([&](RedisKvTxn &transaction) {
    ++attempts;
    auto status = transaction.Set(key, "possibly-committed");
    if (!status.ok()) {
      return status;
    }

    // Keep this control connection alive while closing every other normal
    // client. The transaction's guarded connection is therefore closed before
    // exec() can obtain an acknowledgement; Commit must conservatively treat
    // that boundary as an unknown mutation outcome and must not replay it.
    const auto killed = control.command<long long>("CLIENT", "KILL", "TYPE", "normal", "SKIPME", "yes");
    EXPECT_GT(killed, 0);
    return utils::Status::OK();
  });

  EXPECT_TRUE(status.IsOutcomeUnknown());
  EXPECT_EQ(status.ToErrno(), EIO);
  EXPECT_NE(status.message().find("ambiguous after EXEC"), std::string::npos);
  EXPECT_EQ(attempts, 1);
  control.del(key);
}

TEST(RedisMetaClientTest, ReadOnlyTransactionCommitsAsNoOp) {
  RedisMetaConfig config;
  if (!ParseTestConfig(&config)) {
    GTEST_SKIP() << "SWORDFS_REDIS_TEST_URL is not configured";
  }

  RedisMetaClient store(config);
  const std::string key = UniqueRedisName("read-only");
  sw::redis::Redis cleanup(ConnectionOptions(config));
  cleanup.set(key, "value");

  const auto status = store.Transact([&](RedisKvTxn &transaction) {
    std::string value;
    return transaction.Get(key, &value);
  });
  EXPECT_TRUE(status.ok()) << status.message();
  cleanup.del(key);
}

TEST(RedisMetaClientTest, KvTransactionValidatesOutputsAndRejectsReadAfterWrite) {
  RedisMetaConfig config;
  if (!ParseTestConfig(&config)) {
    GTEST_SKIP() << "SWORDFS_REDIS_TEST_URL is not configured";
  }

  RedisMetaClient store(config);
  const std::string key = UniqueRedisName("kv-validation");
  const auto status = store.Transact([&](RedisKvTxn &txn) {
    EXPECT_EQ(txn.Get(key, nullptr).ToErrno(), EINVAL);
    EXPECT_EQ(txn.HGet(key, "field", nullptr).ToErrno(), EINVAL);
    EXPECT_EQ(txn.HLen(key, nullptr).ToErrno(), EINVAL);
    uint64_t next_cursor = 0;
    std::vector<std::pair<std::string, std::string>> values;
    EXPECT_EQ(txn.HScan(key, 0, 16, nullptr, &next_cursor).ToErrno(), EINVAL);
    EXPECT_EQ(txn.HScan(key, 0, 16, &values, nullptr).ToErrno(), EINVAL);

    auto status = txn.Set(key, "value");
    if (!status.ok()) {
      return status;
    }
    std::string value;
    uint64_t length = 0;
    EXPECT_EQ(txn.Get(key, &value).ToErrno(), EINVAL);
    EXPECT_EQ(txn.HGet(key, "field", &value).ToErrno(), EINVAL);
    EXPECT_EQ(txn.HLen(key, &length).ToErrno(), EINVAL);
    EXPECT_EQ(txn.HScan(key, 0, 16, &values, &next_cursor).ToErrno(), EINVAL);
    return utils::Status::OK();
  });
  EXPECT_TRUE(status.ok()) << status.message();

  sw::redis::Redis cleanup(ConnectionOptions(config));
  cleanup.del(key);
}

TEST(RedisMetaClientTest, KvTransactionCommitsHashCounterAndDeleteMutations) {
  RedisMetaConfig config;
  if (!ParseTestConfig(&config)) {
    GTEST_SKIP() << "SWORDFS_REDIS_TEST_URL is not configured";
  }

  RedisMetaClient store(config);
  const std::string hash_key = UniqueRedisName("kv-hash");
  const std::string counter_key = UniqueRedisName("kv-counter");
  const std::string deleted_key = UniqueRedisName("kv-delete");
  sw::redis::Redis redis(ConnectionOptions(config));
  redis.del(hash_key);
  redis.set(counter_key, "10");
  redis.set(deleted_key, "value");
  redis.hset(hash_key, "old", "old-value");

  const auto status = store.Transact([&](RedisKvTxn &txn) {
    std::string value;
    auto status = txn.HGet(hash_key, "old", &value);
    if (!status.ok()) {
      return status;
    }
    EXPECT_EQ(value, "old-value");
    uint64_t length = 0;
    status = txn.HLen(hash_key, &length);
    if (!status.ok()) {
      return status;
    }
    EXPECT_EQ(length, 1U);

    status = txn.HSet(hash_key, "new", "new-value");
    if (!status.ok()) {
      return status;
    }
    status = txn.HDel(hash_key, "old");
    if (!status.ok()) {
      return status;
    }
    status = txn.IncrBy(counter_key, 5);
    if (!status.ok()) {
      return status;
    }
    return txn.Del(deleted_key);
  });
  ASSERT_TRUE(status.ok()) << status.message();
  EXPECT_EQ(redis.hget(hash_key, "new").value_or(""), "new-value");
  EXPECT_FALSE(redis.hexists(hash_key, "old"));
  EXPECT_EQ(redis.get(counter_key).value_or(""), "15");
  EXPECT_FALSE(redis.exists(deleted_key));

  redis.del(hash_key);
  redis.del(counter_key);
}

TEST(RedisMetaClientTest, PreservesCallbackErrorWithoutQueuedWrite) {
  RedisMetaConfig config;
  if (!ParseTestConfig(&config)) {
    GTEST_SKIP() << "SWORDFS_REDIS_TEST_URL is not configured";
  }

  RedisMetaClient store(config);
  const std::string key = UniqueRedisName("callback-missing");
  const auto status = store.Transact([&](RedisKvTxn &transaction) {
    std::string value;
    const auto get_status = transaction.Get(key, &value);
    if (!get_status.ok()) {
      return get_status;
    }
    return utils::Status::NotFound("expected missing key");
  });
  EXPECT_TRUE(status.IsNotFound()) << status.message();
}

TEST(RedisMetaOpsTest, BackendContextMayBeReleasedByFiberAfterThreadShutdown) {
  RedisMetaConfig config;
  config.host = "127.0.0.1";
  config.retry_attempts = 1;

  auto ops = std::make_unique<RedisMetaOps>(config, UniqueRedisName("lifecycle"));
  std::shared_ptr<DirIterator> iterator;
  RunInFiber([&] {
    ASSERT_TRUE(ops->CreateDirIterator(kRootInodeId, {}, &iterator).ok());
    ASSERT_NE(iterator, nullptr);
  });

  // RedisMetaOps is the thread-domain lifecycle owner. Its destructor drains
  // and clears the blocking backend resources while the iterator may still be
  // alive. Releasing the last iterator/context reference on a fiber must then
  // be safe because no blocking resource remains to be destroyed there.
  ops.reset();
  RunInFiber([&] { iterator.reset(); });
}

#ifndef NDEBUG
TEST(RedisMetaClientTest, RejectsFiberDomainCalls) {
  RedisMetaConfig config;
  config.host = "127.0.0.1";
  RedisMetaClient store(config);
  const std::string key = UniqueRedisName("wrong-domain");

  EXPECT_DEATH(
      {
        RunInFiber([&] {
          std::string value;
          (void)store.Get(key, &value);
        });
      },
      "execution-domain violation at .*expected=POSIX-thread, actual=fiber");
}
#endif

TEST(RedisBackendContextTest, RequiresOwnerShutdownBeforeDestruction) {
  RedisMetaConfig config;
  config.host = "127.0.0.1";
  config.retry_attempts = 1;

  EXPECT_DEATH(
      {
        RedisBackendContext backend(config, 1);
        (void)backend.client();
      },
      "must be shut down by its thread-domain owner");
}

TEST(RedisBackendContextTest, ShutdownIsIdempotent) {
  RedisMetaConfig config;
  config.host = "127.0.0.1";
  config.retry_attempts = 1;

  RedisBackendContext backend(config, 1);
  EXPECT_NE(&backend.client(), nullptr);
  EXPECT_NE(&backend.executor(), nullptr);
  backend.Shutdown();
  backend.Shutdown();
}

TEST(RedisBackendContextTest, RejectsAccessAfterShutdown) {
  RedisMetaConfig config;
  config.host = "127.0.0.1";
  config.retry_attempts = 1;

  EXPECT_DEATH(
      {
        RedisBackendContext backend(config, 1);
        backend.Shutdown();
        (void)backend.client();
      },
      "Redis backend is shut down");

  EXPECT_DEATH(
      {
        RedisBackendContext backend(config, 1);
        backend.Shutdown();
        (void)backend.executor();
      },
      "Redis backend is shut down");
}

TEST(RedisMetaOpsTest, TransactionCallbackRunsInThreadDomain) {
  RedisMetaConfig config;
  if (!ParseTestConfig(&config)) {
    GTEST_SKIP() << "SWORDFS_REDIS_TEST_URL is not configured";
  }

  RedisMetaOps ops(config, UniqueRedisName("callback-domain"));
  utils::ExecutionDomain callback_domain = utils::ExecutionDomain::kFiber;
  const auto status = RunInFiber([&] {
    return ops.TransactFromFiber([&](RedisMetaTxn &) {
      callback_domain = utils::CurrentExecutionDomain();
      return utils::Status::OK();
    });
  });

  ASSERT_TRUE(status.ok()) << status.message();
  EXPECT_EQ(callback_domain, utils::ExecutionDomain::kThread);
}

TEST(RedisMetaOpsTest, GetInodeUsesDirectMetadataReadPath) {
  RedisMetaConfig config;
  if (!ParseTestConfig(&config)) {
    GTEST_SKIP() << "SWORDFS_REDIS_TEST_URL is not configured";
  }

  const std::string volume_name = UniqueRedisName("ops-get-inode");
  RedisMetaOps ops(config, volume_name);
  const redis::RedisKey key(config.db, volume_name);
  sw::redis::Redis redis(ConnectionOptions(config));

  SwordFsAttr attr(42, S_IFREG | 0644);
  attr.size = 1234;
  SwordFsInode inode(42, attr, kRootInodeId);
  ASSERT_TRUE(SeedInode(redis, key, inode).ok());

  SwordFsInode loaded;
  const auto status = RunInFiber([&] { return ops.GetInode(inode.ino, &loaded); });
  ASSERT_TRUE(status.ok()) << status.message();
  EXPECT_EQ(loaded.ino, inode.ino);
  EXPECT_EQ(loaded.attr.size, inode.attr.size);

  const auto invalid_status = RunInFiber([&] { return ops.GetInode(inode.ino, nullptr); });
  EXPECT_EQ(invalid_status.ToErrno(), EINVAL);
}

TEST(RedisMetaClientTest, MGetValidatesOutputAndEmptyInput) {
  RedisMetaConfig config;
  if (!ParseTestConfig(&config)) {
    GTEST_SKIP() << "SWORDFS_REDIS_TEST_URL is not configured";
  }

  RedisMetaClient client(config);
  std::vector<std::optional<std::string>> values{std::string("stale")};
  ASSERT_TRUE(client.MGet({}, &values).ok());
  EXPECT_TRUE(values.empty());
  EXPECT_EQ(client.MGet({"unused"}, nullptr).ToErrno(), EINVAL);
}

TEST(RedisMetaClientTest, MGetReportsConnectionFailure) {
  RedisMetaConfig config;
  config.host = "127.0.0.1";
  config.port = 1;
  config.retry_attempts = 1;

  RedisMetaClient client(config);
  std::vector<std::optional<std::string>> values;
  const auto status = client.MGet({"unreachable"}, &values);
  EXPECT_TRUE(status.IsUnavailable()) << status.message();
}

TEST(RedisMetaClientTest, StandaloneReadOnlyConnectionFailuresReturnUnavailable) {
  RedisMetaConfig config;
  config.host = "127.0.0.1";
  config.port = 1;
  config.connect_timeout = std::chrono::milliseconds(50);
  config.socket_timeout = std::chrono::milliseconds(50);
  config.pool_wait_timeout = std::chrono::milliseconds(50);
  config.retry_attempts = 1;

  RedisMetaClient client(config);
  EXPECT_TRUE(client.Ping().IsUnavailable());

  std::string value;
  EXPECT_TRUE(client.Get("unreachable", &value).IsUnavailable());
  EXPECT_TRUE(client.HGet("unreachable", "field", &value).IsUnavailable());

  std::vector<std::optional<std::string>> values;
  EXPECT_TRUE(client.MGet({"unreachable"}, &values).IsUnavailable());

  std::vector<std::pair<std::string, std::string>> hash_values;
  uint64_t next_cursor = 0;
  EXPECT_TRUE(client.HScan("unreachable", 0, 16, &hash_values, &next_cursor).IsUnavailable());
}

TEST(RedisMetaOpsTest, GetInodesUsesAlignedBatchReadWithMissingEntries) {
  RedisMetaConfig config;
  if (!ParseTestConfig(&config)) {
    GTEST_SKIP() << "SWORDFS_REDIS_TEST_URL is not configured";
  }

  const std::string volume_name = UniqueRedisName("ops-get-inodes");
  RedisMetaOps ops(config, volume_name);
  const redis::RedisKey key(config.db, volume_name);
  sw::redis::Redis redis(ConnectionOptions(config));

  SwordFsInode first(42, SwordFsAttr(42, S_IFREG | 0644), kRootInodeId);
  first.attr.size = 1234;
  SwordFsInode second(43, SwordFsAttr(43, S_IFDIR | 0750), kRootInodeId);
  second.attr.nlink = 2;
  ASSERT_TRUE(SeedInode(redis, key, first).ok());
  ASSERT_TRUE(SeedInode(redis, key, second).ok());

  std::vector<std::optional<SwordFsInode>> results;
  const auto status = RunInFiber([&] { return ops.GetInodes({first.ino, 999999, second.ino}, &results); });
  ASSERT_TRUE(status.ok()) << status.message();
  ASSERT_EQ(results.size(), 3u);
  ASSERT_TRUE(results[0].has_value());
  EXPECT_EQ(results[0]->attr.size, first.attr.size);
  EXPECT_FALSE(results[1].has_value());
  ASSERT_TRUE(results[2].has_value());
  EXPECT_EQ(results[2]->attr.nlink, second.attr.nlink);

  results.emplace_back(first);
  ASSERT_TRUE(RunInFiber([&] { return ops.GetInodes({}, &results); }).ok());
  EXPECT_TRUE(results.empty());

  const auto invalid_status = RunInFiber([&] { return ops.GetInodes({first.ino}, nullptr); });
  EXPECT_EQ(invalid_status.ToErrno(), EINVAL);
}

TEST(RedisMetaOpsTest, GetInodesPropagatesConnectionFailure) {
  RedisMetaConfig config;
  config.host = "127.0.0.1";
  config.port = 1;
  config.retry_attempts = 1;

  RedisMetaOps ops(config, "unreachable-get-inodes");
  std::vector<std::optional<SwordFsInode>> results;
  const auto status = RunInFiber([&] { return ops.GetInodes({42}, &results); });
  EXPECT_EQ(status.ToErrno(), EIO) << status.message();
}

TEST(RedisMetaOpsTest, GetInodesRejectsMalformedAndMismatchedRecords) {
  RedisMetaConfig config;
  if (!ParseTestConfig(&config)) {
    GTEST_SKIP() << "SWORDFS_REDIS_TEST_URL is not configured";
  }

  const std::string volume_name = UniqueRedisName("ops-get-inodes-invalid");
  RedisMetaOps ops(config, volume_name);
  const redis::RedisKey key(config.db, volume_name);
  sw::redis::Redis redis(ConnectionOptions(config));

  constexpr InodeID kMalformedIno = 44;
  redis.set(key.Inode(kMalformedIno), "not-an-inode");
  std::vector<std::optional<SwordFsInode>> results;
  EXPECT_TRUE(RunInFiber([&] { return ops.GetInodes({kMalformedIno}, &results); }).ToErrno() == EIO);

  constexpr InodeID kRequestedIno = 46;
  SwordFsInode other(45, SwordFsAttr(45, S_IFREG | 0644), kRootInodeId);
  std::string encoded;
  ASSERT_TRUE(other.SerializeTo(&encoded).ok());
  redis.set(key.Inode(kRequestedIno), encoded);
  EXPECT_TRUE(RunInFiber([&] { return ops.GetInodes({kRequestedIno}, &results); }).ToErrno() == EIO);
}

TEST(RedisMetaOpsTest, LookupEntryOwnsReadOnlyTransaction) {
  RedisMetaConfig config;
  if (!ParseTestConfig(&config)) {
    GTEST_SKIP() << "SWORDFS_REDIS_TEST_URL is not configured";
  }

  const std::string volume_name = UniqueRedisName("ops-lookup-entry");
  RedisMetaOps ops(config, volume_name);
  const redis::RedisKey key(config.db, volume_name);
  sw::redis::Redis redis(ConnectionOptions(config));

  SwordFsAttr root_attr(kRootInodeId, S_IFDIR | 0755);
  SwordFsInode root(kRootInodeId, root_attr, kRootInodeId);
  SwordFsAttr child_attr(2, S_IFREG | 0644);
  SwordFsInode child(2, child_attr, kRootInodeId);
  ASSERT_TRUE(SeedInode(redis, key, root).ok());
  ASSERT_TRUE(SeedInode(redis, key, child).ok());
  ASSERT_TRUE(SeedEntry(redis, key, kRootInodeId, SwordFsEntry{"child", DT_REG, child.ino}).ok());

  SwordFsInode loaded;
  const auto status = RunInFiber([&] { return ops.LookupEntry(kRootInodeId, "child", &loaded); });
  ASSERT_TRUE(status.ok()) << status.message();
  EXPECT_EQ(loaded.ino, child.ino);

  const auto invalid_status = RunInFiber([&] { return ops.LookupEntry(kRootInodeId, "child", nullptr); });
  EXPECT_EQ(invalid_status.ToErrno(), EINVAL);
}

TEST(RedisMetaTxnTest, PrivateIndexPublicationCommitsAndRejectsWithLogicalHead) {
  RedisMetaConfig config;
  if (!ParseTestConfig(&config)) {
    GTEST_SKIP() << "SWORDFS_REDIS_TEST_URL is not configured";
  }

  RedisMetaClient store(config);
  const redis::RedisKey key(config.db, UniqueRedisName("private-index-publication"));
  sw::redis::Redis redis(ConnectionOptions(config));
  SwordFsAttr file_attr(9, S_IFREG | 0644);
  SwordFsInode file(9, file_attr, kRootInodeId);
  ASSERT_TRUE(SeedInode(redis, key, file).ok());
  RecordingRedisStrategy strategy;

  std::optional<PendingDelete> last_cleanup;
  auto publish = [&](const std::optional<SwordFsChunk> &expected, const SwordFsChunk &replacement,
                     const ChunkPublishIntent &intent = {}) {
    utils::Status publication_result;
    std::optional<PendingDelete> cleanup_candidate;
    auto status = store.Transact([&](RedisKvTxn &kv_txn) {
      RedisMetaTxn txn(kv_txn, key, 4096, &strategy);
      return txn.CommitChunk(file.ino, expected, replacement, publication_result, cleanup_candidate, intent);
    });
    last_cleanup = std::move(cleanup_candidate);
    return status.ok() ? publication_result : status;
  };

  const SwordFsChunk first{.index = 0, .revision = 1, .size = 64};
  ASSERT_TRUE(publish(std::nullopt, first, ChunkPublishIntent{.payload = "manifest-one"}).ok());
  const auto private_hash = key.PrivateChunkIndex(strategy.mechanism(), "fragments:" + std::to_string(file.ino));
  EXPECT_EQ(redis.hget(private_hash, "1"), std::optional<std::string>{"manifest-one"});
  std::string private_value;
  std::vector<std::pair<std::string, std::string>> fields;
  ASSERT_TRUE(store
                  .Transact([&](RedisKvTxn &kv_txn) {
                    RedisMetaTxn txn(kv_txn, key, 4096, &strategy);
                    auto status = txn.Read("fragments:" + std::to_string(file.ino), "1", &private_value);
                    if (!status.ok()) {
                      return status;
                    }
                    status = txn.Scan("fragments:" + std::to_string(file.ino), &fields);
                    if (!status.ok()) {
                      return status;
                    }
                    ChunkView view;
                    status = txn.LoadChunkView(file.ino, 0, &view);
                    if (!status.ok()) {
                      return status;
                    }
                    EXPECT_EQ(view.head, first);
                    EXPECT_EQ(view.private_snapshot, "manifest-one");
                    return utils::Status::OK();
                  })
                  .ok());
  EXPECT_EQ(private_value, "manifest-one");
  EXPECT_EQ(fields, (std::vector<std::pair<std::string, std::string>>{{"1", "manifest-one"}}));

  const SwordFsChunk replacement{.index = 0, .revision = 2, .size = 96};
  strategy.index.reject_publish = true;
  EXPECT_EQ(publish(first, replacement, ChunkPublishIntent{.payload = "manifest-two"}).ToErrno(), EIO);
  ASSERT_TRUE(last_cleanup.has_value());
  EXPECT_FALSE(redis.hget(private_hash, "2").has_value());
  SwordFsChunk head;
  const auto encoded = redis.hget(key.Chunk(file.ino), "0");
  ASSERT_TRUE(encoded.has_value());
  ASSERT_TRUE(head.ParseFrom(*encoded).ok());
  EXPECT_EQ(head, first);
}

TEST(RedisMetaTxnTest, EntryMutationsCarryStateThroughParameters) {
  RedisMetaConfig config;
  if (!ParseTestConfig(&config)) {
    GTEST_SKIP() << "SWORDFS_REDIS_TEST_URL is not configured";
  }

  RedisMetaClient store(config);
  const redis::RedisKey key(config.db, UniqueRedisName("entries"));
  sw::redis::Redis redis(ConnectionOptions(config));

  SwordFsAttr root_attr(kRootInodeId, S_IFDIR | 0755);
  SwordFsInode root(kRootInodeId, root_attr, kRootInodeId);
  ASSERT_TRUE(SeedInode(redis, key, root).ok());
  redis.set(key.InodeCount(), "1");

  const auto status = store.Transact([&](RedisKvTxn &kv_txn) {
    RedisMetaTxn txn(kv_txn, key, 4096);

    SwordFsAttr child_attr(2, S_IFDIR | 0755);
    SwordFsInode child(2, child_attr, kRootInodeId);
    return txn.AddEntry(kRootInodeId, "child", child, &root);
  });
  ASSERT_TRUE(status.ok()) << status.message();

  EXPECT_TRUE(redis.hexists(key.Directory(kRootInodeId), "child"));
  EXPECT_EQ(redis.get(key.InodeCount()).value_or(""), "2");

  std::string value = redis.get(key.Inode(kRootInodeId)).value_or("");
  SwordFsInode persisted_root;
  ASSERT_TRUE(persisted_root.ParseFrom(value).ok());
  EXPECT_EQ(persisted_root.attr.nlink, 3U);
}

TEST(RedisMetaTxnTest, AddEntryRejectsExistingNameWithoutPersistingChild) {
  RedisMetaConfig config;
  if (!ParseTestConfig(&config)) {
    GTEST_SKIP() << "SWORDFS_REDIS_TEST_URL is not configured";
  }

  RedisMetaClient store(config);
  const redis::RedisKey key(config.db, UniqueRedisName("add-entry-duplicate"));
  sw::redis::Redis redis(ConnectionOptions(config));

  SwordFsAttr root_attr(kRootInodeId, S_IFDIR | 0755);
  SwordFsInode root(kRootInodeId, root_attr, kRootInodeId);
  SwordFsAttr existing_attr(7, S_IFREG | 0644);
  SwordFsInode existing(7, existing_attr, kRootInodeId);
  ASSERT_TRUE(SeedInode(redis, key, root).ok());
  ASSERT_TRUE(SeedInode(redis, key, existing).ok());
  ASSERT_TRUE(SeedEntry(redis, key, kRootInodeId, SwordFsEntry{"child", DT_REG, existing.ino}).ok());
  redis.set(key.InodeCount(), "2");

  SwordFsAttr child_attr(8, S_IFREG | 0644);
  SwordFsInode child(8, child_attr, kRootInodeId);
  const auto status = store.Transact([&](RedisKvTxn &kv_txn) {
    RedisMetaTxn txn(kv_txn, key, 4096);
    return txn.AddEntry(kRootInodeId, "child", child, &root);
  });
  EXPECT_TRUE(status.ToErrno() == EEXIST);
  EXPECT_FALSE(redis.exists(key.Inode(child.ino)));
  EXPECT_EQ(redis.get(key.InodeCount()).value_or(""), "2");
}

TEST(RedisMetaTxnTest, AddEntryFailsClosedOnDanglingExistingEntry) {
  RedisMetaConfig config;
  if (!ParseTestConfig(&config)) {
    GTEST_SKIP() << "SWORDFS_REDIS_TEST_URL is not configured";
  }

  RedisMetaClient store(config);
  const redis::RedisKey key(config.db, UniqueRedisName("add-entry-dangling"));
  sw::redis::Redis redis(ConnectionOptions(config));

  SwordFsAttr root_attr(kRootInodeId, S_IFDIR | 0755);
  SwordFsInode root(kRootInodeId, root_attr, kRootInodeId);
  ASSERT_TRUE(SeedInode(redis, key, root).ok());
  ASSERT_TRUE(SeedEntry(redis, key, root.ino, SwordFsEntry{"child", DT_REG, 99}).ok());

  SwordFsAttr child_attr(8, S_IFREG | 0644);
  SwordFsInode child(8, child_attr, root.ino);
  const auto status = store.Transact([&](RedisKvTxn &kv_txn) {
    RedisMetaTxn txn(kv_txn, key, 4096);
    return txn.AddEntry(root.ino, "child", child, &root);
  });

  EXPECT_EQ(status.ToErrno(), EIO) << status.message();
  EXPECT_TRUE(redis.hexists(key.Directory(root.ino), "child"));
  EXPECT_FALSE(redis.exists(key.Inode(child.ino)));
}

TEST(RedisMetaTxnTest, AddEntryPropagatesCorruptDirectoryBackendWithoutMutation) {
  RedisMetaConfig config;
  if (!ParseTestConfig(&config)) {
    GTEST_SKIP() << "SWORDFS_REDIS_TEST_URL is not configured";
  }

  RedisMetaClient store(config);
  const redis::RedisKey key(config.db, UniqueRedisName("add-entry-wrong-type"));
  sw::redis::Redis redis(ConnectionOptions(config));

  SwordFsAttr root_attr(kRootInodeId, S_IFDIR | 0755);
  SwordFsInode root(kRootInodeId, root_attr, kRootInodeId);
  ASSERT_TRUE(SeedInode(redis, key, root).ok());
  redis.set(key.Directory(root.ino), "not-a-directory-hash");

  SwordFsAttr child_attr(8, S_IFREG | 0644);
  SwordFsInode child(8, child_attr, root.ino);
  const auto status = store.Transact([&](RedisKvTxn &kv_txn) {
    RedisMetaTxn txn(kv_txn, key, 4096);
    return txn.AddEntry(root.ino, "child", child, &root);
  });

  EXPECT_EQ(status.ToErrno(), EIO) << status.message();
  EXPECT_EQ(redis.get(key.Directory(root.ino)).value_or(""), "not-a-directory-hash");
  EXPECT_FALSE(redis.exists(key.Inode(child.ino)));
}

TEST(RedisMetaTxnTest, MoveEntryPersistsExplicitState) {
  RedisMetaConfig config;
  if (!ParseTestConfig(&config)) {
    GTEST_SKIP() << "SWORDFS_REDIS_TEST_URL is not configured";
  }

  RedisMetaClient store(config);
  const redis::RedisKey key(config.db, UniqueRedisName("move-entry"));
  sw::redis::Redis redis(ConnectionOptions(config));

  SwordFsAttr root_attr(kRootInodeId, S_IFDIR | 0755);
  SwordFsInode root(kRootInodeId, root_attr, kRootInodeId);
  SwordFsAttr child_attr(2, S_IFREG | 0644);
  SwordFsInode child(2, child_attr, kRootInodeId);
  ASSERT_TRUE(SeedInode(redis, key, root).ok());
  ASSERT_TRUE(SeedInode(redis, key, child).ok());
  ASSERT_TRUE(SeedEntry(redis, key, kRootInodeId, SwordFsEntry{"file", DT_REG, child.ino}).ok());

  const auto status = store.Transact([&](RedisKvTxn &kv_txn) {
    RedisMetaTxn txn(kv_txn, key, 4096);
    return txn.MoveEntry(kRootInodeId, "file", kRootInodeId, "moved", &root, &root, &child, nullptr,
                         /*overwrite=*/true);
  });
  ASSERT_TRUE(status.ok()) << status.message();
  EXPECT_FALSE(redis.hexists(key.Directory(kRootInodeId), "file"));
  EXPECT_TRUE(redis.hexists(key.Directory(kRootInodeId), "moved"));
}

TEST(RedisMetaTxnTest, RemoveDirectoryPersistsLifecycleState) {
  RedisMetaConfig config;
  if (!ParseTestConfig(&config)) {
    GTEST_SKIP() << "SWORDFS_REDIS_TEST_URL is not configured";
  }

  RedisMetaClient store(config);
  const redis::RedisKey key(config.db, UniqueRedisName("remove-directory"));
  sw::redis::Redis redis(ConnectionOptions(config));

  SwordFsAttr root_attr(kRootInodeId, S_IFDIR | 0755);
  root_attr.nlink = 3;
  SwordFsInode root(kRootInodeId, root_attr, kRootInodeId);
  SwordFsAttr dir_attr(2, S_IFDIR | 0755);
  SwordFsInode dir(2, dir_attr, kRootInodeId);
  ASSERT_TRUE(SeedInode(redis, key, root).ok());
  ASSERT_TRUE(SeedInode(redis, key, dir).ok());
  ASSERT_TRUE(SeedEntry(redis, key, kRootInodeId, SwordFsEntry{"dir", DT_DIR, dir.ino}).ok());
  redis.set(key.InodeCount(), "2");

  const auto status = store.Transact([&](RedisKvTxn &kv_txn) {
    RedisMetaTxn txn(kv_txn, key, 4096);
    return txn.RemoveDirectory(kRootInodeId, "dir", &root, dir);
  });
  ASSERT_TRUE(status.ok()) << status.message();
  EXPECT_FALSE(redis.hexists(key.Directory(kRootInodeId), "dir"));
  EXPECT_FALSE(redis.exists(key.Inode(dir.ino)));
  EXPECT_EQ(redis.get(key.InodeCount()).value_or(""), "1");

  std::string value = redis.get(key.Inode(kRootInodeId)).value_or("");
  SwordFsInode persisted_root;
  ASSERT_TRUE(persisted_root.ParseFrom(value).ok());
  EXPECT_EQ(persisted_root.attr.nlink, 2U);
}

TEST(RedisMetaTxnTest, RemoveDirectoryPropagatesCorruptChildDirectoryWithoutMutation) {
  RedisMetaConfig config;
  if (!ParseTestConfig(&config)) {
    GTEST_SKIP() << "SWORDFS_REDIS_TEST_URL is not configured";
  }

  RedisMetaClient store(config);
  const redis::RedisKey key(config.db, UniqueRedisName("remove-directory-wrong-type"));
  sw::redis::Redis redis(ConnectionOptions(config));

  SwordFsAttr root_attr(kRootInodeId, S_IFDIR | 0755);
  root_attr.nlink = 3;
  SwordFsInode root(kRootInodeId, root_attr, kRootInodeId);
  SwordFsAttr dir_attr(2, S_IFDIR | 0755);
  SwordFsInode dir(2, dir_attr, root.ino);
  ASSERT_TRUE(SeedInode(redis, key, root).ok());
  ASSERT_TRUE(SeedInode(redis, key, dir).ok());
  ASSERT_TRUE(SeedEntry(redis, key, root.ino, SwordFsEntry{"dir", DT_DIR, dir.ino}).ok());
  redis.set(key.Directory(dir.ino), "not-a-directory-hash");

  const auto status = store.Transact([&](RedisKvTxn &kv_txn) {
    RedisMetaTxn txn(kv_txn, key, 4096);
    return txn.RemoveDirectory(root.ino, "dir", &root, dir);
  });

  EXPECT_EQ(status.ToErrno(), EIO) << status.message();
  EXPECT_TRUE(redis.hexists(key.Directory(root.ino), "dir"));
  EXPECT_TRUE(redis.exists(key.Inode(dir.ino)));
  EXPECT_EQ(redis.get(key.Directory(dir.ino)).value_or(""), "not-a-directory-hash");
}

TEST(RedisMetaTxnTest, ReadPrimitivesValidateOutputs) {
  RedisMetaConfig config;
  if (!ParseTestConfig(&config)) {
    GTEST_SKIP() << "SWORDFS_REDIS_TEST_URL is not configured";
  }

  RedisMetaClient store(config);
  const redis::RedisKey key(config.db, UniqueRedisName("read-primitives"));
  sw::redis::Redis redis(ConnectionOptions(config));

  SwordFsAttr file_attr(2, S_IFREG | 0644);
  SwordFsInode file(2, file_attr, kRootInodeId);
  ASSERT_TRUE(SeedInode(redis, key, file).ok());

  const auto status = store.Transact([&](RedisKvTxn &kv_txn) {
    RedisMetaTxn txn(kv_txn, key, 4096);
    EXPECT_EQ(txn.LookupInode(file.ino, nullptr).ToErrno(), EINVAL);

    SwordFsInode missing;
    auto status = txn.LookupEntry(file.ino, "missing", &missing);
    EXPECT_TRUE(status.ToErrno() == ENOTDIR);
    EXPECT_EQ(txn.LookupEntry(file.ino, "missing", nullptr).ToErrno(), EINVAL);

    return utils::Status::OK();
  });
  EXPECT_TRUE(status.ok()) << status.message();
}

TEST(RedisMetaTxnTest, NamespaceMutationPrimitivesValidateStateContracts) {
  RedisMetaConfig config;
  if (!ParseTestConfig(&config)) {
    GTEST_SKIP() << "SWORDFS_REDIS_TEST_URL is not configured";
  }

  RedisMetaClient store(config);
  const redis::RedisKey key(config.db, UniqueRedisName("namespace-validation"));
  sw::redis::Redis redis(ConnectionOptions(config));

  SwordFsAttr dir_attr(10, S_IFDIR | 0755);
  dir_attr.nlink = 2;
  SwordFsInode dir(10, dir_attr, kRootInodeId);
  SwordFsAttr other_dir_attr(11, S_IFDIR | 0755);
  other_dir_attr.nlink = 2;
  SwordFsInode other_dir(11, other_dir_attr, kRootInodeId);
  SwordFsAttr file_attr(20, S_IFREG | 0644);
  file_attr.nlink = 1;
  SwordFsInode file(20, file_attr, dir.ino);
  SwordFsAttr other_file_attr(21, S_IFREG | 0644);
  other_file_attr.nlink = 1;
  SwordFsInode other_file(21, other_file_attr, dir.ino);

  const auto status = store.Transact([&](RedisKvTxn &kv_txn) {
    RedisMetaTxn txn(kv_txn, key, 4096);

    EXPECT_EQ(txn.LookupEntry(dir, "entry", nullptr).ToErrno(), EINVAL);

    EXPECT_EQ(txn.AddEntry(dir.ino, "entry", file, nullptr).ToErrno(), EINVAL);
    auto wrong_child_parent = file;
    wrong_child_parent.parent_ino = other_dir.ino;
    EXPECT_EQ(txn.AddEntry(dir.ino, "entry", wrong_child_parent, &dir).ToErrno(), EINVAL);
    auto wrong_parent = dir;
    wrong_parent.ino = other_dir.ino;
    EXPECT_EQ(txn.AddEntry(dir.ino, "entry", file, &wrong_parent).ToErrno(), EINVAL);

    EXPECT_EQ(txn.UnlinkFile(dir.ino, "entry", nullptr, &file).ToErrno(), EINVAL);
    EXPECT_EQ(txn.UnlinkFile(dir.ino, "entry", &dir, nullptr).ToErrno(), EINVAL);
    EXPECT_EQ(txn.UnlinkFile(other_dir.ino, "entry", &dir, &file).ToErrno(), EINVAL);
    EXPECT_EQ(txn.UnlinkFile(dir.ino, "entry", &dir, &other_dir).ToErrno(), EINVAL);

    EXPECT_EQ(txn.RemoveDirectory(dir.ino, "entry", nullptr, other_dir).ToErrno(), EINVAL);
    EXPECT_EQ(txn.RemoveDirectory(other_dir.ino, "entry", &dir, other_dir).ToErrno(), EINVAL);
    EXPECT_EQ(txn.RemoveDirectory(dir.ino, "entry", &dir, file).ToErrno(), ENOTDIR);

    EXPECT_EQ(txn.MoveEntry(dir.ino, "old", dir.ino, "new", nullptr, &dir, &file, nullptr, false).ToErrno(), EINVAL);
    EXPECT_EQ(txn.MoveEntry(dir.ino, "old", dir.ino, "new", &dir, nullptr, &file, nullptr, false).ToErrno(), EINVAL);
    EXPECT_EQ(txn.MoveEntry(dir.ino, "old", dir.ino, "new", &dir, &dir, nullptr, nullptr, false).ToErrno(), EINVAL);
    EXPECT_EQ(txn.MoveEntry(other_dir.ino, "old", dir.ino, "new", &dir, &dir, &file, nullptr, false).ToErrno(), EINVAL);
    EXPECT_EQ(txn.MoveEntry(dir.ino, "old", other_dir.ino, "new", &dir, &dir, &file, nullptr, false).ToErrno(), EINVAL);
    auto same_id_parent = dir;
    EXPECT_EQ(txn.MoveEntry(dir.ino, "old", dir.ino, "new", &dir, &same_id_parent, &file, nullptr, false).ToErrno(),
              EINVAL);
    EXPECT_EQ(txn.MoveEntry(file.ino, "old", file.ino, "new", &file, &file, &other_file, nullptr, false).ToErrno(),
              ENOTDIR);
    EXPECT_EQ(txn.MoveEntry(dir.ino, "old", dir.ino, "new", &dir, &dir, &file, &other_file, false).ToErrno(), EEXIST);
    auto same_file = file;
    EXPECT_TRUE(txn.MoveEntry(dir.ino, "old", dir.ino, "new", &dir, &dir, &file, &same_file, true).ok());

    EXPECT_EQ(txn.ExchangeEntries(dir.ino, "old", dir.ino, "new", nullptr, &dir, &file, &other_file).ToErrno(), EINVAL);
    EXPECT_EQ(txn.ExchangeEntries(dir.ino, "old", dir.ino, "new", &dir, nullptr, &file, &other_file).ToErrno(), EINVAL);
    EXPECT_EQ(txn.ExchangeEntries(dir.ino, "old", dir.ino, "new", &dir, &dir, nullptr, &other_file).ToErrno(), EINVAL);
    EXPECT_EQ(txn.ExchangeEntries(dir.ino, "old", dir.ino, "new", &dir, &dir, &file, nullptr).ToErrno(), EINVAL);
    EXPECT_EQ(txn.ExchangeEntries(other_dir.ino, "old", dir.ino, "new", &dir, &dir, &file, &other_file).ToErrno(),
              EINVAL);
    EXPECT_EQ(txn.ExchangeEntries(dir.ino, "old", other_dir.ino, "new", &dir, &dir, &file, &other_file).ToErrno(),
              EINVAL);
    EXPECT_EQ(txn.ExchangeEntries(dir.ino, "old", dir.ino, "new", &dir, &same_id_parent, &file, &other_file).ToErrno(),
              EINVAL);
    EXPECT_EQ(txn.ExchangeEntries(file.ino, "old", file.ino, "new", &file, &file, &file, &other_file).ToErrno(),
              ENOTDIR);
    auto exchange_same_file = file;
    EXPECT_TRUE(txn.ExchangeEntries(dir.ino, "old", dir.ino, "new", &dir, &dir, &file, &exchange_same_file).ok());

    EXPECT_EQ(txn.LinkExistingEntry(dir.ino, "link", nullptr, &file).ToErrno(), EINVAL);
    EXPECT_EQ(txn.LinkExistingEntry(dir.ino, "link", &dir, nullptr).ToErrno(), EINVAL);
    EXPECT_EQ(txn.LinkExistingEntry(other_dir.ino, "link", &dir, &file).ToErrno(), EINVAL);
    EXPECT_EQ(txn.LinkExistingEntry(file.ino, "link", &file, &other_file).ToErrno(), ENOTDIR);
    EXPECT_EQ(txn.LinkExistingEntry(dir.ino, "link", &dir, &other_dir).ToErrno(), EPERM);

    return utils::Status::OK();
  });
  ASSERT_TRUE(status.ok()) << status.message();
  EXPECT_EQ(redis.hlen(key.Directory(dir.ino)), 0);
  EXPECT_EQ(redis.hlen(key.Directory(other_dir.ino)), 0);
  EXPECT_FALSE(redis.exists(key.Inode(dir.ino)));
  EXPECT_FALSE(redis.exists(key.Inode(other_dir.ino)));
  EXPECT_FALSE(redis.exists(key.Inode(file.ino)));
  EXPECT_FALSE(redis.exists(key.Inode(other_file.ino)));
}

TEST(RedisMetaTxnTest, AddEntryRejectsInvalidInodeIdentityWithoutMutation) {
  RedisMetaConfig config;
  if (!ParseTestConfig(&config)) {
    GTEST_SKIP() << "SWORDFS_REDIS_TEST_URL is not configured";
  }

  RedisMetaClient store(config);
  const redis::RedisKey key(config.db, UniqueRedisName("invalid-inode-identity"));
  sw::redis::Redis redis(ConnectionOptions(config));

  SwordFsAttr parent_attr(kRootInodeId, S_IFDIR | 0755);
  parent_attr.nlink = 2;
  SwordFsInode parent(kRootInodeId, parent_attr, kRootInodeId);

  const auto status = store.Transact([&](RedisKvTxn &kv_txn) {
    RedisMetaTxn txn(kv_txn, key, 4096);

    SwordFsAttr zero_attr(0, S_IFREG | 0644);
    SwordFsInode zero_ino(0, zero_attr, parent.ino);
    EXPECT_EQ(txn.AddEntry(parent.ino, "zero", zero_ino, &parent).ToErrno(), EINVAL);

    SwordFsAttr mismatched_attr(31, S_IFREG | 0644);
    SwordFsInode mismatched(30, mismatched_attr, parent.ino);
    EXPECT_EQ(txn.AddEntry(parent.ino, "mismatch", mismatched, &parent).ToErrno(), EINVAL);
    return utils::Status::OK();
  });

  ASSERT_TRUE(status.ok()) << status.message();
  EXPECT_EQ(redis.hlen(key.Directory(parent.ino)), 0);
  EXPECT_FALSE(redis.exists(key.Inode(30)));
  EXPECT_FALSE(redis.exists(key.Inode(31)));
}

TEST(RedisMetaTxnTest, ChunkPublicContractsRejectInvalidDescriptorsAndViews) {
  RedisMetaConfig config;
  if (!ParseTestConfig(&config)) {
    GTEST_SKIP() << "SWORDFS_REDIS_TEST_URL is not configured";
  }

  RedisMetaClient store(config);
  const redis::RedisKey key(config.db, UniqueRedisName("chunk-contract-validation"));
  sw::redis::Redis redis(ConnectionOptions(config));
  RecordingRedisStrategy strategy;
  constexpr InodeID kFileIno = 40;

  const auto validation_status = store.Transact([&](RedisKvTxn &kv_txn) {
    RedisMetaTxn txn(kv_txn, key, 4096, &strategy);
    utils::Status publication_result;
    std::optional<PendingDelete> cleanup_candidate;

    const SwordFsChunk replacement{.index = 0, .revision = 2, .size = 64};

    const SwordFsChunk same_revision{.index = 0, .revision = 2, .size = 64};
    EXPECT_EQ(txn.CommitChunk(kFileIno, same_revision, replacement, publication_result, cleanup_candidate).ToErrno(),
              EINVAL);

    const SwordFsChunk expected{.index = 0, .revision = 1, .size = 64};
    const SwordFsChunk different_identity{.index = 1, .revision = 2, .size = 64};
    EXPECT_EQ(txn.CommitChunk(kFileIno, expected, different_identity, publication_result, cleanup_candidate).ToErrno(),
              EINVAL);

    EXPECT_EQ(txn.LoadChunkView(kFileIno, 0, nullptr).ToErrno(), EINVAL);
    return utils::Status::OK();
  });
  ASSERT_TRUE(validation_status.ok()) << validation_status.message();

  SwordFsChunk wrong_identity{.index = 0, .revision = 3, .size = 64};
  std::string encoded;
  ASSERT_TRUE(wrong_identity.SerializeTo(&encoded).ok());
  redis.hset(key.Chunk(kFileIno), "1", encoded);
  const auto malformed_status = store.Transact([&](RedisKvTxn &kv_txn) {
    RedisMetaTxn txn(kv_txn, key, 4096, &strategy);
    ChunkView view;
    return txn.LoadChunkView(kFileIno, 1, &view);
  });
  EXPECT_EQ(malformed_status.ToErrno(), EIO) << malformed_status.message();

  redis.del(key.Chunk(kFileIno));
  SwordFsChunk head{.index = 0, .revision = 4, .size = 64};
  ASSERT_TRUE(head.SerializeTo(&encoded).ok());
  redis.hset(key.Chunk(kFileIno), "0", encoded);
  const auto private_state_status = store.Transact([&](RedisKvTxn &kv_txn) {
    RedisMetaTxn txn(kv_txn, key, 4096, &strategy);
    ChunkView view;
    return txn.LoadChunkView(kFileIno, 0, &view);
  });
  EXPECT_TRUE(private_state_status.IsNotFound()) << private_state_status.message();
}

TEST(RedisMetaTxnTest, TouchInodePropagatesMissingInode) {
  RedisMetaConfig config;
  if (!ParseTestConfig(&config)) {
    GTEST_SKIP() << "SWORDFS_REDIS_TEST_URL is not configured";
  }

  RedisMetaClient store(config);
  const redis::RedisKey key(config.db, UniqueRedisName("touch-missing-inode"));
  const auto status = store.Transact([&](RedisKvTxn &kv_txn) {
    RedisMetaTxn txn(kv_txn, key, 4096);
    return txn.TouchInode(424242, SetAttrField::kCtime);
  });

  EXPECT_TRUE(status.IsNotFound()) << status.message();
}

TEST(RedisMetaTxnTest, PrivateChunkIndexPrimitivesValidateTheirPublicContract) {
  RedisMetaConfig config;
  if (!ParseTestConfig(&config)) {
    GTEST_SKIP() << "SWORDFS_REDIS_TEST_URL is not configured";
  }

  RedisMetaClient store(config);
  const redis::RedisKey key(config.db, UniqueRedisName("private-index-contract"));
  sw::redis::Redis redis(ConnectionOptions(config));
  RecordingRedisStrategy strategy;

  const auto validation_status = store.Transact([&](RedisKvTxn &kv_txn) {
    RedisMetaTxn txn(kv_txn, key, 4096, &strategy);
    std::string value;
    std::vector<std::pair<std::string, std::string>> values;
    EXPECT_EQ(txn.Read("", "field", &value).ToErrno(), EINVAL);
    EXPECT_EQ(txn.Read("manifest", "field", nullptr).ToErrno(), EINVAL);
    EXPECT_EQ(txn.Scan("", &values).ToErrno(), EINVAL);
    EXPECT_EQ(txn.Scan("manifest", nullptr).ToErrno(), EINVAL);
    EXPECT_EQ(txn.Put("", "field", "value").ToErrno(), EINVAL);
    EXPECT_EQ(txn.Erase("", "field").ToErrno(), EINVAL);
    return utils::Status::OK();
  });
  ASSERT_TRUE(validation_status.ok()) << validation_status.message();

  const auto put_status = store.Transact([&](RedisKvTxn &kv_txn) {
    RedisMetaTxn txn(kv_txn, key, 4096, &strategy);
    return txn.Put("manifest", "b", "two");
  });
  ASSERT_TRUE(put_status.ok()) << put_status.message();
  redis.hset(key.PrivateChunkIndex(strategy.mechanism(), "manifest"), "a", "one");

  const auto read_status = store.Transact([&](RedisKvTxn &kv_txn) {
    RedisMetaTxn txn(kv_txn, key, 4096, &strategy);
    std::string value;
    auto status = txn.Read("manifest", "b", &value);
    EXPECT_TRUE(status.ok()) << status.message();
    EXPECT_EQ(value, "two");

    std::vector<std::pair<std::string, std::string>> values;
    status = txn.Scan("manifest", &values);
    EXPECT_TRUE(status.ok()) << status.message();
    EXPECT_EQ(values, (std::vector<std::pair<std::string, std::string>>{{"a", "one"}, {"b", "two"}}));
    return utils::Status::OK();
  });
  ASSERT_TRUE(read_status.ok()) << read_status.message();

  const auto erase_status = store.Transact([&](RedisKvTxn &kv_txn) {
    RedisMetaTxn txn(kv_txn, key, 4096, &strategy);
    return txn.Erase("manifest", "a");
  });
  ASSERT_TRUE(erase_status.ok()) << erase_status.message();
  EXPECT_FALSE(redis.hexists(key.PrivateChunkIndex(strategy.mechanism(), "manifest"), "a"));
  EXPECT_EQ(redis.hget(key.PrivateChunkIndex(strategy.mechanism(), "manifest"), "b").value_or(""), "two");
}

TEST(RedisMetaTxnTest, PrivateChunkIndexScanFailsClosedOnCorruptBackendType) {
  RedisMetaConfig config;
  if (!ParseTestConfig(&config)) {
    GTEST_SKIP() << "SWORDFS_REDIS_TEST_URL is not configured";
  }

  RedisMetaClient store(config);
  const redis::RedisKey key(config.db, UniqueRedisName("private-index-wrong-type"));
  sw::redis::Redis redis(ConnectionOptions(config));
  RecordingRedisStrategy strategy;
  redis.set(key.PrivateChunkIndex(strategy.mechanism(), "manifest"), "not-a-hash");

  const auto status = store.Transact([&](RedisKvTxn &kv_txn) {
    RedisMetaTxn txn(kv_txn, key, 4096, &strategy);
    std::vector<std::pair<std::string, std::string>> values;
    return txn.Scan("manifest", &values);
  });

  EXPECT_EQ(status.ToErrno(), EIO) << status.message();
  EXPECT_EQ(redis.get(key.PrivateChunkIndex(strategy.mechanism(), "manifest")).value_or(""), "not-a-hash");
}

TEST(RedisMetaTxnTest, MoveEntryRejectsCorruptDirectoryAncestryWithoutMutation) {
  RedisMetaConfig config;
  if (!ParseTestConfig(&config)) {
    GTEST_SKIP() << "SWORDFS_REDIS_TEST_URL is not configured";
  }

  RedisMetaClient store(config);
  const redis::RedisKey key(config.db, UniqueRedisName("rename-parent-cycle"));
  sw::redis::Redis redis(ConnectionOptions(config));

  SwordFsAttr root_attr(kRootInodeId, S_IFDIR | 0755);
  root_attr.nlink = 3;
  SwordFsInode root(kRootInodeId, root_attr, kRootInodeId);
  SwordFsAttr source_attr(2, S_IFDIR | 0755);
  source_attr.nlink = 2;
  SwordFsInode source(2, source_attr, kRootInodeId);
  SwordFsAttr first_cycle_attr(3, S_IFDIR | 0755);
  SwordFsInode first_cycle(3, first_cycle_attr, 4);
  SwordFsAttr second_cycle_attr(4, S_IFDIR | 0755);
  SwordFsInode second_cycle(4, second_cycle_attr, 3);

  ASSERT_TRUE(SeedInode(redis, key, root).ok());
  ASSERT_TRUE(SeedInode(redis, key, source).ok());
  ASSERT_TRUE(SeedInode(redis, key, first_cycle).ok());
  ASSERT_TRUE(SeedInode(redis, key, second_cycle).ok());
  ASSERT_TRUE(SeedEntry(redis, key, root.ino, SwordFsEntry{"source", DT_DIR, source.ino}).ok());

  const auto status = store.Transact([&](RedisKvTxn &kv_txn) {
    RedisMetaTxn txn(kv_txn, key, 4096);
    return txn.MoveEntry(root.ino, "source", first_cycle.ino, "moved", &root, &first_cycle, &source, nullptr,
                         /*overwrite=*/false);
  });

  EXPECT_EQ(status.ToErrno(), EIO) << status.message();
  EXPECT_TRUE(redis.hexists(key.Directory(root.ino), "source"));
  EXPECT_FALSE(redis.hexists(key.Directory(first_cycle.ino), "moved"));
  EXPECT_TRUE(redis.get(key.Inode(source.ino)).has_value());
}

TEST(RedisMetaTxnTest, MoveEntryRejectsMissingDirectoryAncestryWithoutMutation) {
  RedisMetaConfig config;
  if (!ParseTestConfig(&config)) {
    GTEST_SKIP() << "SWORDFS_REDIS_TEST_URL is not configured";
  }

  RedisMetaClient store(config);
  const redis::RedisKey key(config.db, UniqueRedisName("rename-missing-parent"));
  sw::redis::Redis redis(ConnectionOptions(config));

  SwordFsAttr root_attr(kRootInodeId, S_IFDIR | 0755);
  root_attr.nlink = 3;
  SwordFsInode root(kRootInodeId, root_attr, kRootInodeId);
  SwordFsAttr source_attr(2, S_IFDIR | 0755);
  source_attr.nlink = 2;
  SwordFsInode source(2, source_attr, root.ino);
  SwordFsAttr destination_attr(3, S_IFDIR | 0755);
  SwordFsInode destination(3, destination_attr, 999);

  ASSERT_TRUE(SeedInode(redis, key, root).ok());
  ASSERT_TRUE(SeedInode(redis, key, source).ok());
  ASSERT_TRUE(SeedInode(redis, key, destination).ok());
  ASSERT_TRUE(SeedEntry(redis, key, root.ino, SwordFsEntry{"source", DT_DIR, source.ino}).ok());

  const auto status = store.Transact([&](RedisKvTxn &kv_txn) {
    RedisMetaTxn txn(kv_txn, key, 4096);
    return txn.MoveEntry(root.ino, "source", destination.ino, "moved", &root, &destination, &source, nullptr,
                         /*overwrite=*/false);
  });

  EXPECT_TRUE(status.IsNotFound()) << status.message();
  EXPECT_TRUE(redis.hexists(key.Directory(root.ino), "source"));
  EXPECT_FALSE(redis.hexists(key.Directory(destination.ino), "moved"));
  EXPECT_TRUE(redis.get(key.Inode(source.ino)).has_value());
}

TEST(RedisMetaTxnTest, LookupEntryRejectsDanglingDirectoryEntry) {
  RedisMetaConfig config;
  if (!ParseTestConfig(&config)) {
    GTEST_SKIP() << "SWORDFS_REDIS_TEST_URL is not configured";
  }

  RedisMetaClient store(config);
  const redis::RedisKey key(config.db, UniqueRedisName("dangling-entry"));
  sw::redis::Redis redis(ConnectionOptions(config));

  SwordFsAttr root_attr(kRootInodeId, S_IFDIR | 0755);
  SwordFsInode root(kRootInodeId, root_attr, kRootInodeId);
  ASSERT_TRUE(SeedInode(redis, key, root).ok());
  ASSERT_TRUE(SeedEntry(redis, key, kRootInodeId, SwordFsEntry{"dangling", DT_REG, 99}).ok());

  const auto status = store.Transact([&](RedisKvTxn &kv_txn) {
    RedisMetaTxn txn(kv_txn, key, 4096);
    SwordFsInode out;
    return txn.LookupEntry(kRootInodeId, "dangling", &out);
  });
  EXPECT_TRUE(status.ToErrno() == EIO) << status.message();
}

TEST(RedisMetaTxnTest, TruncateClampsPersistedBoundaryChunk) {
  RedisMetaConfig config;
  if (!ParseTestConfig(&config)) {
    GTEST_SKIP() << "SWORDFS_REDIS_TEST_URL is not configured";
  }

  RedisMetaClient store(config);
  const redis::RedisKey key(config.db, UniqueRedisName("truncate-staged"));
  sw::redis::Redis redis(ConnectionOptions(config));

  SwordFsAttr file_attr(9, S_IFREG | 0644);
  file_attr.size = 8192;
  SwordFsInode file(9, file_attr, kRootInodeId);
  ASSERT_TRUE(SeedInode(redis, key, file).ok());

  SwordFsChunk first_chunk{0, 1, 4096};
  SwordFsChunk second_chunk{1, 2, 4096};
  std::string first_data;
  std::string second_data;
  ASSERT_TRUE(first_chunk.SerializeTo(&first_data).ok());
  ASSERT_TRUE(second_chunk.SerializeTo(&second_data).ok());
  redis.hset(key.Chunk(9), "0", first_data);
  redis.hset(key.Chunk(9), "1", second_data);

  std::vector<PendingDelete> detached;
  const auto status = store.Transact([&](RedisKvTxn &kv_txn) {
    RedisMetaTxn txn(kv_txn, key, 4096);
    return txn.Truncate(9, 1024, &detached);
  });
  ASSERT_TRUE(status.ok()) << status.message();
  ASSERT_EQ(detached.size(), 1U);
  swordfs::chunk::WholeObjectRef detached_ref;
  ASSERT_TRUE(swordfs::chunk::DecodeWholeObjectDelete(detached.front(), 4096, &detached_ref).ok());
  EXPECT_EQ(detached_ref.descriptor, second_chunk);

  const auto first_value = redis.hget(key.Chunk(9), "0");
  ASSERT_TRUE(first_value.has_value());
  SwordFsChunk first;
  ASSERT_TRUE(first.ParseFrom(*first_value).ok());
  EXPECT_EQ(first.size, 1024U);
  EXPECT_FALSE(redis.hexists(key.Chunk(9), "1"));

  file.attr.size = 4096;
  ASSERT_TRUE(SeedInode(redis, key, file).ok());
  const auto invalid_status = store.Transact([&](RedisKvTxn &kv_txn) {
    RedisMetaTxn txn(kv_txn, key, 0);
    EXPECT_EQ(txn.Truncate(9, 1024).ToErrno(), EIO);
    return utils::Status::OK();
  });
  EXPECT_TRUE(invalid_status.ok()) << invalid_status.message();
}

TEST(RedisMetaTxnTest, SetAttrShrinkWorksWithoutDetachedChunkOutput) {
  RedisMetaConfig config;
  if (!ParseTestConfig(&config)) {
    GTEST_SKIP() << "SWORDFS_REDIS_TEST_URL is not configured";
  }

  RedisMetaClient store(config);
  const redis::RedisKey key(config.db, UniqueRedisName("setattr-no-cleanup-output"));
  sw::redis::Redis redis(ConnectionOptions(config));

  SwordFsAttr file_attr(9, S_IFREG | 0644);
  file_attr.size = 4096;
  SwordFsInode file(9, file_attr, kRootInodeId);
  ASSERT_TRUE(SeedInode(redis, key, file).ok());

  SwordFsChunk chunk{.index = 0, .revision = 1, .size = 4096};
  std::string encoded;
  ASSERT_TRUE(chunk.SerializeTo(&encoded).ok());
  redis.hset(key.Chunk(file.ino), "0", encoded);

  SwordFsAttr requested = file.attr;
  requested.size = 0;
  const auto status = store.Transact([&](RedisKvTxn &kv_txn) {
    RedisMetaTxn txn(kv_txn, key, 4096);
    return txn.SetAttr(file.ino, requested, SetAttrField::kSize);
  });
  ASSERT_TRUE(status.ok()) << status.message();
  EXPECT_FALSE(redis.hexists(key.Chunk(file.ino), "0"));

  SwordFsInode stored;
  ASSERT_TRUE(stored.ParseFrom(redis.get(key.Inode(file.ino)).value_or("")).ok());
  EXPECT_EQ(stored.attr.size, 0U);
}

TEST(RedisMetaTxnTest, SetAttrSizeAndOwnerChangesDoNotInventKillpriv) {
  RedisMetaConfig config;
  if (!ParseTestConfig(&config)) {
    GTEST_SKIP() << "SWORDFS_REDIS_TEST_URL is not configured";
  }

  RedisMetaClient store(config);
  const redis::RedisKey key(config.db, UniqueRedisName("setattr-no-implicit-killpriv"));
  sw::redis::Redis redis(ConnectionOptions(config));

  SwordFsAttr file_attr(9, S_IFREG | 0644 | S_ISUID | S_ISGID, 1000, 100);
  SwordFsInode file(9, file_attr, kRootInodeId);
  ASSERT_TRUE(SeedInode(redis, key, file).ok());

  SwordFsAttr requested = file.attr;
  requested.uid = 2000;
  requested.gid = 200;
  requested.size = 1024;
  const auto status = store.Transact([&](RedisKvTxn &kv_txn) {
    RedisMetaTxn txn(kv_txn, key, 4096);
    return txn.SetAttr(file.ino, requested, SetAttrField::kUid | SetAttrField::kGid | SetAttrField::kSize);
  });
  ASSERT_TRUE(status.ok()) << status.message();

  SwordFsInode stored;
  ASSERT_TRUE(stored.ParseFrom(redis.get(key.Inode(file.ino)).value_or("")).ok());
  EXPECT_EQ(stored.attr.uid, requested.uid);
  EXPECT_EQ(stored.attr.gid, requested.gid);
  EXPECT_EQ(stored.attr.size, requested.size);
  EXPECT_NE(stored.attr.mode & S_ISUID, 0u);
  EXPECT_NE(stored.attr.mode & S_ISGID, 0u);
}

TEST(RedisMetaTxnTest, SetAttrExplicitKillprivUsesModeAwareSgidRule) {
  RedisMetaConfig config;
  if (!ParseTestConfig(&config)) {
    GTEST_SKIP() << "SWORDFS_REDIS_TEST_URL is not configured";
  }

  RedisMetaClient store(config);
  const redis::RedisKey key(config.db, UniqueRedisName("setattr-explicit-killpriv"));
  sw::redis::Redis redis(ConnectionOptions(config));

  SwordFsAttr file_attr(9, S_IFREG | 0644 | S_ISUID | S_ISGID);
  SwordFsInode file(9, file_attr, kRootInodeId);
  ASSERT_TRUE(SeedInode(redis, key, file).ok());

  auto status = store.Transact([&](RedisKvTxn &kv_txn) {
    RedisMetaTxn txn(kv_txn, key, 4096);
    return txn.SetAttr(file.ino, file.attr, kKillSuidGidField);
  });
  ASSERT_TRUE(status.ok()) << status.message();

  SwordFsInode stored;
  ASSERT_TRUE(stored.ParseFrom(redis.get(key.Inode(file.ino)).value_or("")).ok());
  EXPECT_EQ(stored.attr.mode & S_ISUID, 0u);
  EXPECT_NE(stored.attr.mode & S_ISGID, 0u);

  file.attr = stored.attr;
  file.attr.mode |= S_ISUID | S_ISGID | S_IXGRP;
  ASSERT_TRUE(SeedInode(redis, key, file).ok());
  status = store.Transact([&](RedisKvTxn &kv_txn) {
    RedisMetaTxn txn(kv_txn, key, 4096);
    return txn.SetAttr(file.ino, file.attr, kKillSuidGidField);
  });
  ASSERT_TRUE(status.ok()) << status.message();
  ASSERT_TRUE(stored.ParseFrom(redis.get(key.Inode(file.ino)).value_or("")).ok());
  EXPECT_EQ(stored.attr.mode & S_ISUID, 0u);
  EXPECT_EQ(stored.attr.mode & S_ISGID, 0u);
}

TEST(RedisMetaTxnTest, SetAttrCombinedModeOwnerAndKillprivUsesFinalRequestedMode) {
  RedisMetaConfig config;
  if (!ParseTestConfig(&config)) {
    GTEST_SKIP() << "SWORDFS_REDIS_TEST_URL is not configured";
  }

  RedisMetaClient store(config);
  const redis::RedisKey key(config.db, UniqueRedisName("setattr-combined-killpriv"));
  sw::redis::Redis redis(ConnectionOptions(config));

  SwordFsAttr file_attr(9, S_IFREG | 0600, 1000, 100);
  SwordFsInode file(9, file_attr, kRootInodeId);
  ASSERT_TRUE(SeedInode(redis, key, file).ok());

  SwordFsAttr requested = file.attr;
  requested.mode = S_IFREG | 0654 | S_ISUID | S_ISGID;
  requested.uid = 2000;
  requested.gid = 200;
  const auto status = store.Transact([&](RedisKvTxn &kv_txn) {
    RedisMetaTxn txn(kv_txn, key, 4096);
    return txn.SetAttr(file.ino, requested,
                       SetAttrField::kMode | SetAttrField::kUid | SetAttrField::kGid | kKillSuidGidField);
  });
  ASSERT_TRUE(status.ok()) << status.message();

  SwordFsInode stored;
  ASSERT_TRUE(stored.ParseFrom(redis.get(key.Inode(file.ino)).value_or("")).ok());
  EXPECT_EQ(stored.attr.uid, requested.uid);
  EXPECT_EQ(stored.attr.gid, requested.gid);
  EXPECT_EQ(stored.attr.mode & 0777, 0654U);
  EXPECT_EQ(stored.attr.mode & S_ISUID, 0u);
  EXPECT_EQ(stored.attr.mode & S_ISGID, 0u);
}

TEST(RedisMetaTxnTest, SetAttrLegacyExplicitModeIsNotSecondGuessed) {
  RedisMetaConfig config;
  if (!ParseTestConfig(&config)) {
    GTEST_SKIP() << "SWORDFS_REDIS_TEST_URL is not configured";
  }

  RedisMetaClient store(config);
  const redis::RedisKey key(config.db, UniqueRedisName("setattr-legacy-mode"));
  sw::redis::Redis redis(ConnectionOptions(config));

  SwordFsAttr file_attr(9, S_IFREG | 0600, 1000, 100);
  SwordFsInode file(9, file_attr, kRootInodeId);
  ASSERT_TRUE(SeedInode(redis, key, file).ok());

  SwordFsAttr requested = file.attr;
  requested.mode = S_IFREG | 0644 | S_ISGID;
  const auto status = store.Transact([&](RedisKvTxn &kv_txn) {
    RedisMetaTxn txn(kv_txn, key, 4096);
    return txn.SetAttr(file.ino, requested, SetAttrField::kMode);
  });
  ASSERT_TRUE(status.ok()) << status.message();

  SwordFsInode stored;
  ASSERT_TRUE(stored.ParseFrom(redis.get(key.Inode(file.ino)).value_or("")).ok());
  EXPECT_EQ(stored.attr.mode, static_cast<uint32_t>(requested.mode));
}

TEST(RedisMetaTxnTest, TruncateDoesNotInventKillpriv) {
  RedisMetaConfig config;
  if (!ParseTestConfig(&config)) {
    GTEST_SKIP() << "SWORDFS_REDIS_TEST_URL is not configured";
  }

  RedisMetaClient store(config);
  const redis::RedisKey key(config.db, UniqueRedisName("truncate-no-implicit-killpriv"));
  sw::redis::Redis redis(ConnectionOptions(config));

  SwordFsAttr file_attr(9, S_IFREG | 0644 | S_ISUID | S_ISGID);
  file_attr.size = 4096;
  SwordFsInode file(9, file_attr, kRootInodeId);
  ASSERT_TRUE(SeedInode(redis, key, file).ok());

  const auto status = store.Transact([&](RedisKvTxn &kv_txn) {
    RedisMetaTxn txn(kv_txn, key, 4096);
    return txn.Truncate(file.ino, 1024);
  });
  ASSERT_TRUE(status.ok()) << status.message();

  SwordFsInode stored;
  ASSERT_TRUE(stored.ParseFrom(redis.get(key.Inode(file.ino)).value_or("")).ok());
  EXPECT_EQ(stored.attr.size, 1024U);
  EXPECT_NE(stored.attr.mode & S_ISUID, 0u);
  EXPECT_NE(stored.attr.mode & S_ISGID, 0u);
}

TEST(RedisMetaTxnTest, CommitChunkDoesNotInventKillpriv) {
  RedisMetaConfig config;
  if (!ParseTestConfig(&config)) {
    GTEST_SKIP() << "SWORDFS_REDIS_TEST_URL is not configured";
  }

  RedisMetaClient store(config);
  const redis::RedisKey key(config.db, UniqueRedisName("commit-no-implicit-killpriv"));
  sw::redis::Redis redis(ConnectionOptions(config));

  SwordFsAttr file_attr(9, S_IFREG | 0644 | S_ISUID | S_ISGID);
  SwordFsInode file(9, file_attr, kRootInodeId);
  ASSERT_TRUE(SeedInode(redis, key, file).ok());

  const SwordFsChunk chunk{.index = 0, .revision = 1, .size = 64};
  const auto status = CommitChunkTxn(store, key, 4096, file.ino, std::nullopt, chunk);
  ASSERT_TRUE(status.ok()) << status.message();

  SwordFsInode stored;
  ASSERT_TRUE(stored.ParseFrom(redis.get(key.Inode(file.ino)).value_or("")).ok());
  EXPECT_NE(stored.attr.mode & S_ISUID, 0u);
  EXPECT_NE(stored.attr.mode & S_ISGID, 0u);
}

TEST(RedisMetaTxnTest, RegisterPendingDeletesRejectsInvalidEnvelope) {
  RedisMetaConfig config;
  if (!ParseTestConfig(&config)) {
    GTEST_SKIP() << "SWORDFS_REDIS_TEST_URL is not configured";
  }

  RedisMetaClient store(config);
  const redis::RedisKey key(config.db, UniqueRedisName("register-invalid-cleanup"));
  sw::redis::Redis redis(ConnectionOptions(config));
  const std::vector<PendingDelete> work{{.id = "", .index_format_version = 1, .payload = "opaque"}};

  const auto status = store.Transact([&](RedisKvTxn &kv_txn) {
    RedisMetaTxn txn(kv_txn, key, 4096);
    return txn.RegisterPendingDeletes(work);
  });
  EXPECT_EQ(status.ToErrno(), EINVAL);
  EXPECT_EQ(redis.hlen(key.PendingDeletes()), 0);
}

TEST(RedisMetaTxnTest, TruncateDoesNotDependOnPendingDeleteState) {
  RedisMetaConfig config;
  if (!ParseTestConfig(&config)) {
    GTEST_SKIP() << "SWORDFS_REDIS_TEST_URL is not configured";
  }

  RedisMetaClient store(config);
  const redis::RedisKey key(config.db, UniqueRedisName("truncate-independent-cleanup"));
  sw::redis::Redis redis(ConnectionOptions(config));

  SwordFsAttr file_attr(9, S_IFREG | 0644);
  file_attr.size = 4096;
  SwordFsInode file(9, file_attr, kRootInodeId);
  ASSERT_TRUE(SeedInode(redis, key, file).ok());

  SwordFsChunk chunk{0, 1, 4096};
  std::string encoded;
  ASSERT_TRUE(chunk.SerializeTo(&encoded).ok());
  redis.hset(key.Chunk(file.ino), "0", encoded);
  redis.set(key.PendingDeletes(), "wrong-type");

  std::vector<PendingDelete> detached;
  const auto status = store.Transact([&](RedisKvTxn &kv_txn) {
    RedisMetaTxn txn(kv_txn, key, 4096);
    return txn.Truncate(file.ino, 0, &detached);
  });

  ASSERT_TRUE(status.ok()) << status.message();
  ASSERT_EQ(detached.size(), 1U);
  swordfs::chunk::WholeObjectRef detached_ref;
  ASSERT_TRUE(swordfs::chunk::DecodeWholeObjectDelete(detached.front(), 4096, &detached_ref).ok());
  EXPECT_EQ(detached_ref.descriptor, chunk);
  EXPECT_FALSE(redis.hexists(key.Chunk(file.ino), "0"));
}

TEST(RedisMetaTxnTest, TruncateScansMultipleChunkHashPagesAndCollectsDetachedChunks) {
  RedisMetaConfig config;
  if (!ParseTestConfig(&config)) {
    GTEST_SKIP() << "SWORDFS_REDIS_TEST_URL is not configured";
  }

  RedisMetaClient store(config);
  const redis::RedisKey key(config.db, UniqueRedisName("truncate-multipage-chunks"));
  sw::redis::Redis redis(ConnectionOptions(config));

  constexpr uint64_t kChunkSize = 4096;
  constexpr uint32_t kChunkCount = 600;
  SwordFsAttr file_attr(9, S_IFREG | 0644);
  file_attr.size = kChunkCount * kChunkSize;
  SwordFsInode file(9, file_attr, kRootInodeId);
  ASSERT_TRUE(SeedInode(redis, key, file).ok());

  for (uint32_t index = 0; index < kChunkCount; ++index) {
    SwordFsChunk chunk{.index = index,

                       .revision = static_cast<uint64_t>(index) + 1,
                       .size = kChunkSize};
    std::string encoded;
    ASSERT_TRUE(chunk.SerializeTo(&encoded).ok());
    redis.hset(key.Chunk(file.ino), std::to_string(index), encoded);
  }

  std::vector<PendingDelete> detached;
  const auto status = store.Transact([&](RedisKvTxn &kv_txn) {
    RedisMetaTxn txn(kv_txn, key, kChunkSize);
    return txn.Truncate(file.ino, 0, &detached);
  });
  ASSERT_TRUE(status.ok()) << status.message();
  EXPECT_EQ(detached.size(), kChunkCount);
  EXPECT_EQ(redis.hlen(key.Chunk(file.ino)), 0);
}

TEST(RedisMetaTxnTest, TruncateRejectsInvalidChunkMetadata) {
  RedisMetaConfig config;
  if (!ParseTestConfig(&config)) {
    GTEST_SKIP() << "SWORDFS_REDIS_TEST_URL is not configured";
  }

  RedisMetaClient store(config);
  const redis::RedisKey key(config.db, UniqueRedisName("truncate-noncanonical-chunk"));
  sw::redis::Redis redis(ConnectionOptions(config));

  SwordFsAttr file_attr(9, S_IFREG | 0644);
  file_attr.size = 4096;
  SwordFsInode file(9, file_attr, kRootInodeId);
  ASSERT_TRUE(SeedInode(redis, key, file).ok());

  SwordFsChunk wrong{.index = 0, .revision = 1, .size = 4097};
  std::string encoded;
  ASSERT_TRUE(wrong.SerializeTo(&encoded).ok());
  redis.hset(key.Chunk(file.ino), "0", encoded);

  const auto status = store.Transact([&](RedisKvTxn &kv_txn) {
    RedisMetaTxn txn(kv_txn, key, 4096);
    return txn.Truncate(file.ino, 0);
  });
  EXPECT_TRUE(status.ToErrno() == EIO) << status.message();
  EXPECT_TRUE(redis.hexists(key.Chunk(file.ino), "0"));

  redis.del(key.Chunk(file.ino));
  SwordFsChunk canonical{.index = 0, .revision = 2, .size = 64};
  ASSERT_TRUE(canonical.SerializeTo(&encoded).ok());
  redis.hset(key.Chunk(file.ino), "1", encoded);
  const auto field_mismatch_status = store.Transact([&](RedisKvTxn &kv_txn) {
    RedisMetaTxn txn(kv_txn, key, 4096);
    return txn.Truncate(file.ino, 0);
  });
  EXPECT_TRUE(field_mismatch_status.ToErrno() == EIO) << field_mismatch_status.message();
}

TEST(RedisMetaTxnTest, TruncatePropagatesWrongTypeChunkMapFromDestructivePhase) {
  RedisMetaConfig config;
  if (!ParseTestConfig(&config)) {
    GTEST_SKIP() << "SWORDFS_REDIS_TEST_URL is not configured";
  }

  RedisMetaClient store(config);
  const redis::RedisKey key(config.db, UniqueRedisName("truncate-wrongtype-chunks"));
  sw::redis::Redis redis(ConnectionOptions(config));

  SwordFsAttr file_attr(9, S_IFREG | 0644);
  file_attr.size = 4096;
  SwordFsInode file(9, file_attr, kRootInodeId);
  ASSERT_TRUE(SeedInode(redis, key, file).ok());
  redis.set(key.Chunk(file.ino), "wrong-type");

  const auto status = store.Transact([&](RedisKvTxn &kv_txn) {
    RedisMetaTxn txn(kv_txn, key, 4096);
    return txn.Truncate(file.ino, 0);
  });
  EXPECT_FALSE(status.ok());

  SwordFsInode after;
  ASSERT_TRUE(after.ParseFrom(redis.get(key.Inode(file.ino)).value_or("")).ok());
  EXPECT_EQ(after.attr.size, file.attr.size);
}

TEST(RedisMetaTxnTest, CommitChunkRejectsCorruptPersistedDescriptor) {
  RedisMetaConfig config;
  if (!ParseTestConfig(&config)) {
    GTEST_SKIP() << "SWORDFS_REDIS_TEST_URL is not configured";
  }

  RedisMetaClient store(config);
  const redis::RedisKey key(config.db, UniqueRedisName("commit-corrupt-current"));
  sw::redis::Redis redis(ConnectionOptions(config));

  SwordFsAttr file_attr(9, S_IFREG | 0644);
  SwordFsInode file(9, file_attr, kRootInodeId);
  ASSERT_TRUE(SeedInode(redis, key, file).ok());

  SwordFsChunk corrupt{.index = 0, .revision = 1, .size = 4097};
  std::string encoded;
  ASSERT_TRUE(corrupt.SerializeTo(&encoded).ok());
  redis.hset(key.Chunk(file.ino), "0", encoded);

  SwordFsChunk replacement{.index = 0, .revision = 2, .size = 64};
  const auto status = CommitChunkTxn(store, key, 4096, file.ino, std::nullopt, replacement);
  EXPECT_TRUE(status.ToErrno() == EIO) << status.message();
}

TEST(RedisMetaTxnTest, CommitChunkReturnsCleanupCandidateWithoutPendingDeleteDependency) {
  RedisMetaConfig config;
  if (!ParseTestConfig(&config)) {
    GTEST_SKIP() << "SWORDFS_REDIS_TEST_URL is not configured";
  }

  RedisMetaClient store(config);
  const redis::RedisKey key(config.db, UniqueRedisName("rewrite-cleanup-candidate"));
  sw::redis::Redis redis(ConnectionOptions(config));

  SwordFsAttr file_attr(9, S_IFREG | 0644);
  file_attr.size = 64;
  SwordFsInode file(9, file_attr, kRootInodeId);
  ASSERT_TRUE(SeedInode(redis, key, file).ok());

  SwordFsChunk expected{.index = 0, .revision = 1, .size = 64};
  std::string encoded;
  ASSERT_TRUE(expected.SerializeTo(&encoded).ok());
  redis.hset(key.Chunk(file.ino), "0", encoded);
  redis.set(key.PendingDeletes(), "wrong-type");

  auto replacement = expected;
  replacement.revision = 2;
  utils::Status publication_result;
  std::optional<PendingDelete> cleanup_candidate;
  const auto status = store.Transact([&](RedisKvTxn &kv_txn) {
    RedisMetaTxn txn(kv_txn, key, 4096);
    return txn.CommitChunk(file.ino, expected, replacement, publication_result, cleanup_candidate);
  });

  ASSERT_TRUE(status.ok()) << status.message();
  EXPECT_TRUE(publication_result.ok()) << publication_result.message();
  ASSERT_TRUE(cleanup_candidate.has_value());
  swordfs::chunk::WholeObjectRef cleanup_ref;
  ASSERT_TRUE(swordfs::chunk::DecodeWholeObjectDelete(*cleanup_candidate, 4096, &cleanup_ref).ok());
  EXPECT_EQ(cleanup_ref.descriptor, expected);
  const auto stored = redis.hget(key.Chunk(file.ino), "0");
  ASSERT_TRUE(stored.has_value());
  SwordFsChunk current;
  ASSERT_TRUE(current.ParseFrom(*stored).ok());
  EXPECT_EQ(current, replacement);
}

TEST(RedisMetaTxnTest, CommitChunkDefiniteRejectionReturnsReplacementAsCleanupCandidate) {
  RedisMetaConfig config;
  if (!ParseTestConfig(&config)) {
    GTEST_SKIP() << "SWORDFS_REDIS_TEST_URL is not configured";
  }

  RedisMetaClient store(config);
  const redis::RedisKey key(config.db, UniqueRedisName("reject-cleanup-candidate"));
  sw::redis::Redis redis(ConnectionOptions(config));

  SwordFsAttr file_attr(9, S_IFREG | 0644);
  file_attr.size = 64;
  SwordFsInode file(9, file_attr, kRootInodeId);
  ASSERT_TRUE(SeedInode(redis, key, file).ok());

  SwordFsChunk current{.index = 0, .revision = 1, .size = 64};
  std::string encoded;
  ASSERT_TRUE(current.SerializeTo(&encoded).ok());
  redis.hset(key.Chunk(file.ino), "0", encoded);
  redis.set(key.PendingDeletes(), "wrong-type");

  auto replacement = current;
  replacement.revision = 2;
  utils::Status publication_result;
  std::optional<PendingDelete> cleanup_candidate;
  const auto status = store.Transact([&](RedisKvTxn &kv_txn) {
    RedisMetaTxn txn(kv_txn, key, 4096);
    return txn.CommitChunk(file.ino, std::nullopt, replacement, publication_result, cleanup_candidate);
  });

  ASSERT_TRUE(status.ok()) << status.message();
  EXPECT_TRUE(publication_result.ToErrno() == EEXIST) << publication_result.message();
  ASSERT_TRUE(cleanup_candidate.has_value());
  swordfs::chunk::WholeObjectRef cleanup_ref;
  ASSERT_TRUE(swordfs::chunk::DecodeWholeObjectDelete(*cleanup_candidate, 4096, &cleanup_ref).ok());
  EXPECT_EQ(cleanup_ref.descriptor, replacement);
}

TEST(RedisMetaTxnTest, CommitChunkRejectsConflictingInitialPublication) {
  RedisMetaConfig config;
  if (!ParseTestConfig(&config)) {
    GTEST_SKIP() << "SWORDFS_REDIS_TEST_URL is not configured";
  }

  RedisMetaClient store(config);
  const redis::RedisKey key(config.db, UniqueRedisName("commit-chunk"));
  sw::redis::Redis redis(ConnectionOptions(config));

  SwordFsAttr file_attr(9, S_IFREG | 0644);
  SwordFsInode file(9, file_attr, kRootInodeId);
  ASSERT_TRUE(SeedInode(redis, key, file).ok());

  SwordFsChunk chunk{0, 1, 4096};
  const auto first_status = CommitChunkTxn(store, key, 4096, 9, std::nullopt, chunk);
  ASSERT_TRUE(first_status.ok()) << first_status.message();

  chunk.revision = 2;
  const auto duplicate_status = CommitChunkTxn(store, key, 4096, 9, std::nullopt, chunk);
  EXPECT_TRUE(duplicate_status.ToErrno() == EEXIST) << duplicate_status.message();

  const auto value = redis.hget(key.Chunk(9), "0");
  ASSERT_TRUE(value.has_value());
  SwordFsChunk persisted;
  ASSERT_TRUE(persisted.ParseFrom(*value).ok());
  EXPECT_EQ(persisted.revision, 1U);
}

TEST(RedisMetaTxnTest, PrepareReclaimScansAuthoritativeChunksInsideTransaction) {
  RedisMetaConfig config;
  if (!ParseTestConfig(&config)) {
    GTEST_SKIP() << "SWORDFS_REDIS_TEST_URL is not configured";
  }

  RedisMetaClient store(config);
  const redis::RedisKey key(config.db, UniqueRedisName("reclaim-authoritative-scan"));
  sw::redis::Redis redis(ConnectionOptions(config));

  SwordFsAttr file_attr(9, S_IFREG | 0644);
  file_attr.nlink = 0;
  SwordFsInode file(9, file_attr, kRootInodeId);
  ASSERT_TRUE(SeedInode(redis, key, file).ok());

  SwordFsChunk chunk{0, 1, 4096};
  std::string chunk_data;
  ASSERT_TRUE(chunk.SerializeTo(&chunk_data).ok());
  redis.hset(key.Chunk(file.ino), "0", chunk_data);

  std::optional<ReclaimWork> work;
  const auto status = store.Transact([&](RedisKvTxn &kv_txn) {
    RedisMetaTxn txn(kv_txn, key, 4096);
    return txn.PrepareReclaim(file.ino, work);
  });

  ASSERT_TRUE(status.ok()) << status.message();
  ASSERT_TRUE(work.has_value());
  std::vector<swordfs::chunk::WholeObjectRef> refs;
  ASSERT_TRUE(swordfs::chunk::DecodeWholeObjectReclaim(*work, 4096, &refs).ok());
  ASSERT_EQ(refs.size(), 1U);
  EXPECT_EQ(refs[0].descriptor, chunk);
  EXPECT_EQ(refs[0].key, swordfs::chunk::FormatChunkObjectKey(file.ino, chunk.index, chunk.revision));
  EXPECT_FALSE(redis.exists(key.Inode(file.ino)));
  EXPECT_FALSE(redis.exists(key.Chunk(file.ino)));
  EXPECT_TRUE(redis.hexists(key.Reclaims(), std::to_string(file.ino)));
}

TEST(RedisMetaTxnTest, LinkIgnoresUnrelatedFrozenWorkChanges) {
  RedisMetaConfig config;
  if (!ParseTestConfig(&config)) {
    GTEST_SKIP() << "SWORDFS_REDIS_TEST_URL is not configured";
  }

  constexpr InodeID kIno = 9;
  constexpr InodeID kUnrelatedIno = 10;
  const redis::RedisKey key(config.db, UniqueRedisName("link-unrelated-reclaim"));
  sw::redis::Redis redis(ConnectionOptions(config));

  SwordFsAttr root_attr(kRootInodeId, S_IFDIR | 0755);
  root_attr.nlink = 2;
  ASSERT_TRUE(SeedInode(redis, key, SwordFsInode(kRootInodeId, root_attr, kRootInodeId)).ok());
  SwordFsAttr file_attr(kIno, S_IFREG | 0644);
  file_attr.nlink = 0;
  ASSERT_TRUE(SeedInode(redis, key, SwordFsInode(kIno, file_attr, kRootInodeId)).ok());
  redis.hset(key.Orphans(), std::to_string(kIno), "1");

  RedisMetaClient store(config);
  int attempts = 0;
  const auto status = store.Transact([&](RedisKvTxn &kv_txn) {
    ++attempts;
    RedisMetaTxn txn(kv_txn, key, 4096);
    SwordFsInode parent;
    auto status = txn.LookupInode(kRootInodeId, &parent);
    if (!status.ok()) {
      return status;
    }
    SwordFsInode file;
    status = txn.LookupInode(kIno, &file);
    if (!status.ok()) {
      return status;
    }
    status = txn.LinkExistingEntry(kRootInodeId, "revived", &parent, &file);
    if (!status.ok()) {
      return status;
    }

    // Change a different field after Link has made every decision but before
    // EXEC. Per-inode reclaim state must not make this unrelated field part
    // of Link's optimistic transaction snapshot.
    redis.hset(key.Reclaims(), std::to_string(kUnrelatedIno), "unrelated-" + std::to_string(attempts));
    return utils::Status::OK();
  });

  ASSERT_TRUE(status.ok()) << status.message();
  EXPECT_EQ(attempts, 1);
  EXPECT_TRUE(redis.hexists(key.Directory(kRootInodeId), "revived"));
}

TEST(RedisMetaTxnTest, PrepareReclaimIgnoresUnrelatedFrozenWorkChanges) {
  RedisMetaConfig config;
  if (!ParseTestConfig(&config)) {
    GTEST_SKIP() << "SWORDFS_REDIS_TEST_URL is not configured";
  }

  constexpr InodeID kIno = 9;
  constexpr InodeID kUnrelatedIno = 10;
  const redis::RedisKey key(config.db, UniqueRedisName("reclaim-unrelated-reclaim"));
  sw::redis::Redis redis(ConnectionOptions(config));

  SwordFsAttr file_attr(kIno, S_IFREG | 0644);
  file_attr.nlink = 0;
  ASSERT_TRUE(SeedInode(redis, key, SwordFsInode(kIno, file_attr, kRootInodeId)).ok());
  redis.hset(key.Orphans(), std::to_string(kIno), "1");

  RedisMetaClient store(config);
  int attempts = 0;
  std::optional<ReclaimWork> work;
  const auto status = store.Transact([&](RedisKvTxn &kv_txn) {
    ++attempts;
    RedisMetaTxn txn(kv_txn, key, 4096);
    auto status = txn.PrepareReclaim(kIno, work);
    if (!status.ok()) {
      return status;
    }
    redis.hset(key.Reclaims(), std::to_string(kUnrelatedIno), "unrelated-" + std::to_string(attempts));
    return utils::Status::OK();
  });

  ASSERT_TRUE(status.ok()) << status.message();
  EXPECT_EQ(attempts, 1);
  ASSERT_TRUE(work.has_value());
  EXPECT_FALSE(redis.exists(key.Inode(kIno)));
  EXPECT_TRUE(redis.hexists(key.Reclaims(), std::to_string(kIno)));
}

TEST(RedisMetaTxnTest, LinkCommitInvalidatesConcurrentReclaimSnapshot) {
  RedisMetaConfig config;
  if (!ParseTestConfig(&config)) {
    GTEST_SKIP() << "SWORDFS_REDIS_TEST_URL is not configured";
  }

  constexpr InodeID kIno = 9;
  const redis::RedisKey key(config.db, UniqueRedisName("link-reclaim-race"));
  sw::redis::Redis redis(ConnectionOptions(config));

  SwordFsAttr root_attr(kRootInodeId, S_IFDIR | 0755);
  root_attr.nlink = 2;
  ASSERT_TRUE(SeedInode(redis, key, SwordFsInode(kRootInodeId, root_attr, kRootInodeId)).ok());
  SwordFsAttr file_attr(kIno, S_IFREG | 0644);
  file_attr.nlink = 0;
  ASSERT_TRUE(SeedInode(redis, key, SwordFsInode(kIno, file_attr, kRootInodeId)).ok());
  redis.hset(key.Orphans(), std::to_string(kIno), "1");
  const SwordFsChunk chunk{.index = 0, .revision = 1, .size = 64};
  std::string encoded_chunk;
  ASSERT_TRUE(chunk.SerializeTo(&encoded_chunk).ok());
  redis.hset(key.Chunk(kIno), "0", encoded_chunk);

  RedisMetaClient reclaim_store(config);
  RedisMetaClient link_store(config);
  int attempts = 0;
  std::optional<ReclaimWork> work;
  const auto status = reclaim_store.Transact([&](RedisKvTxn &kv_txn) {
    ++attempts;
    RedisMetaTxn txn(kv_txn, key, 4096);
    auto status = txn.PrepareReclaim(kIno, work);
    if (!status.ok()) {
      return status;
    }
    if (attempts != 1) {
      return utils::Status::OK();
    }

    return link_store.Transact([&](RedisKvTxn &link_kv_txn) {
      RedisMetaTxn link_txn(link_kv_txn, key, 4096);
      SwordFsInode parent;
      auto status = link_txn.LookupInode(kRootInodeId, &parent);
      if (!status.ok()) {
        return status;
      }
      SwordFsInode file;
      status = link_txn.LookupInode(kIno, &file);
      if (!status.ok()) {
        return status;
      }
      return link_txn.LinkExistingEntry(kRootInodeId, "revived", &parent, &file);
    });
  });

  ASSERT_TRUE(status.ok()) << status.message();
  EXPECT_EQ(attempts, 2);
  EXPECT_FALSE(work.has_value());
  EXPECT_TRUE(redis.hexists(key.Directory(kRootInodeId), "revived"));
  EXPECT_TRUE(redis.exists(key.Inode(kIno)));
  EXPECT_TRUE(redis.exists(key.Chunk(kIno)));
  EXPECT_FALSE(redis.hexists(key.Reclaims(), std::to_string(kIno)));
}

TEST(RedisMetaTxnTest, PrepareReclaimConvergesAfterAmbiguousExec) {
  RedisMetaConfig config;
  if (!ParseTestConfig(&config)) {
    GTEST_SKIP() << "SWORDFS_REDIS_TEST_URL is not configured";
  }

  constexpr InodeID kIno = 9;
  const redis::RedisKey key(config.db, UniqueRedisName("ambiguous-reclaim"));
  sw::redis::Redis control(ConnectionOptions(config));

  SwordFsAttr file_attr(kIno, S_IFREG | 0644);
  file_attr.nlink = 0;
  ASSERT_TRUE(SeedInode(control, key, SwordFsInode(kIno, file_attr, kRootInodeId)).ok());
  control.hset(key.Orphans(), std::to_string(kIno), "1");
  const SwordFsChunk chunk{.index = 0, .revision = 7, .size = 128};
  std::string encoded_chunk;
  ASSERT_TRUE(chunk.SerializeTo(&encoded_chunk).ok());
  control.hset(key.Chunk(kIno), "0", encoded_chunk);

  config.socket_timeout = std::chrono::milliseconds(50);
  config.retry_attempts = 3;
  config.retry_backoff = std::chrono::milliseconds(0);
  RedisMetaClient first_store(config);
  int attempts = 0;
  std::optional<ReclaimWork> first_work;
  const auto first_status = first_store.Transact([&](RedisKvTxn &kv_txn) {
    ++attempts;
    RedisMetaTxn txn(kv_txn, key, 4096);
    auto status = txn.PrepareReclaim(kIno, first_work);
    if (!status.ok()) {
      return status;
    }
    control.command<void>("CLIENT", "PAUSE", 1000, "ALL");
    return utils::Status::OK();
  });
  control.command<void>("CLIENT", "UNPAUSE");

  EXPECT_EQ(first_status.ToErrno(), EIO);
  EXPECT_NE(first_status.message().find("ambiguous after EXEC"), std::string::npos);
  EXPECT_EQ(attempts, 1);

  RedisMetaConfig retry_config;
  ASSERT_TRUE(ParseTestConfig(&retry_config));
  RedisMetaClient retry_store(retry_config);
  std::optional<ReclaimWork> replay;
  const auto retry_status = retry_store.Transact([&](RedisKvTxn &kv_txn) {
    RedisMetaTxn txn(kv_txn, key, 4096);
    return txn.PrepareReclaim(kIno, replay);
  });

  ASSERT_TRUE(retry_status.ok()) << retry_status.message();
  ASSERT_TRUE(replay.has_value());
  std::vector<swordfs::chunk::WholeObjectRef> refs;
  ASSERT_TRUE(swordfs::chunk::DecodeWholeObjectReclaim(*replay, 4096, &refs).ok());
  ASSERT_EQ(refs.size(), 1U);
  EXPECT_EQ(refs[0].descriptor, chunk);
  EXPECT_FALSE(control.exists(key.Inode(kIno)));
  EXPECT_FALSE(control.exists(key.Chunk(kIno)));
  EXPECT_TRUE(control.hexists(key.Reclaims(), std::to_string(kIno)));
}

}  // namespace swordfs::metadata
