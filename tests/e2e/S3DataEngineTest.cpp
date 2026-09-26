// Copyright 2026 SwordFS Contributors.
// Licensed under the Apache License, Version 2.0.

#include <folly/io/IOBuf.h>
#include <gtest/gtest.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>

#include <array>
#include <chrono>
#include <cstdlib>
#include <cstring>
#include <memory>
#include <stdexcept>
#include <string>
#include <thread>

#include "storage/s3/S3DataEngine.hpp"
#include "tests/unittests/FiberTest.hpp"

namespace swordfs::storage {
namespace {

using namespace std::chrono_literals;

class TricklingS3Server {
 public:
  TricklingS3Server() {
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
    if (::listen(listen_fd_, 4) != 0) {
      throw std::runtime_error(std::string("listen: ") + std::strerror(errno));
    }

    socklen_t len = sizeof(addr);
    if (::getsockname(listen_fd_, reinterpret_cast<sockaddr *>(&addr), &len) != 0) {
      throw std::runtime_error(std::string("getsockname: ") + std::strerror(errno));
    }
    port_ = ntohs(addr.sin_port);
    thread_ = std::thread([this] { Serve(); });
  }

  ~TricklingS3Server() {
    if (listen_fd_ >= 0) {
      ::shutdown(listen_fd_, SHUT_RDWR);
      ::close(listen_fd_);
    }
    if (thread_.joinable()) {
      thread_.join();
    }
  }

  uint16_t port() const {
    return port_;
  }

 private:
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

  void Serve() {
    const int client = ::accept4(listen_fd_, nullptr, nullptr, SOCK_CLOEXEC);
    if (client < 0) {
      return;
    }

    std::string request;
    std::array<char, 1024> buffer{};
    while (request.find("\r\n\r\n") == std::string::npos) {
      const auto n = ::recv(client, buffer.data(), buffer.size(), 0);
      if (n <= 0) {
        ::close(client);
        return;
      }
      request.append(buffer.data(), static_cast<size_t>(n));
    }

    constexpr size_t kPayloadSize = 128;
    const std::string headers = "HTTP/1.1 200 OK\r\nContent-Length: " + std::to_string(kPayloadSize) +
                                "\r\nContent-Type: application/octet-stream\r\nConnection: close\r\n\r\n";
    if (!SendAll(client, headers)) {
      ::close(client);
      return;
    }

    // Keep making observable forward progress well above Curl's 1 byte/s
    // low-speed threshold while taking long enough to require an independent
    // absolute request deadline.
    for (size_t i = 0; i < kPayloadSize; ++i) {
      if (!SendAll(client, "x")) {
        break;
      }
      std::this_thread::sleep_for(50ms);
    }
    ::close(client);
  }

  int listen_fd_ = -1;
  uint16_t port_ = 0;
  std::thread thread_;
};

class S3DataEngineE2ETest : public ::testing::Test {
 protected:
  void SetUp() override {
    const char *bucket = std::getenv("SWORDFS_E2E_S3_BUCKET");
    ASSERT_NE(bucket, nullptr);
    ASSERT_NE(bucket[0], '\0');

    auto *test_info = ::testing::UnitTest::GetInstance()->current_test_info();
    prefix_ = "direct-s3-" + std::to_string(::getpid()) + "/" + test_info->name();

    std::string location(bucket);
    if (!location.empty() && location.back() != '/') {
      location += '/';
    }
    location += prefix_;

    DataEngineOptions options{.location = std::move(location), .worker_count = 2};
    if (const char *region = std::getenv("AWS_DEFAULT_REGION")) {
      options.region = region;
    }

    engine_ = std::make_unique<S3DataEngine>(std::move(options));
    const auto status = engine_->Initialize();
    ASSERT_TRUE(status.ok()) << status.message();
  }

  Status Put(std::string_view key, std::string_view value) {
    Status status;
    swordfs::test::RunInTestFiber([&] { status = engine_->Put(key, folly::IOBuf::copyBuffer(value)); });
    return status;
  }

  Status Get(std::string_view key, size_t offset, size_t size, folly::IOBuf *out) {
    Status status;
    swordfs::test::RunInTestFiber([&] { status = engine_->Get(key, offset, size, out); });
    return status;
  }

  Status Delete(std::string_view key) {
    Status status;
    swordfs::test::RunInTestFiber([&] { status = engine_->Delete(key); });
    return status;
  }

  std::unique_ptr<S3DataEngine> engine_;
  std::string prefix_;
};

TEST_F(S3DataEngineE2ETest, SurfacesMissingBucketWriteAndDeleteFailures) {
  const char *bucket = std::getenv("SWORDFS_E2E_S3_BUCKET");
  ASSERT_NE(bucket, nullptr);
  ASSERT_NE(bucket[0], '\0');

  std::string missing_location(bucket);
  while (!missing_location.empty() && missing_location.back() == '/') {
    missing_location.pop_back();
  }
  missing_location += "-missing-" + std::to_string(::getpid());

  DataEngineOptions options{.location = std::move(missing_location), .worker_count = 1};
  if (const char *region = std::getenv("AWS_DEFAULT_REGION")) {
    options.region = region;
  }

  S3DataEngine missing_bucket(std::move(options));
  ASSERT_TRUE(missing_bucket.Initialize().ok());

  Status put_status;
  swordfs::test::RunInTestFiber(
      [&] { put_status = missing_bucket.Put("object", folly::IOBuf::copyBuffer("payload")); });
  EXPECT_EQ(put_status.ToErrno(), EIO) << put_status.message();

  Status delete_status;
  swordfs::test::RunInTestFiber([&] { delete_status = missing_bucket.Delete("object"); });
  EXPECT_EQ(delete_status.ToErrno(), EIO) << delete_status.message();
}

TEST_F(S3DataEngineE2ETest, RoundTripsRangesMissingObjectsAndIdempotentDelete) {
  constexpr std::string_view kKey = "contract-object";
  constexpr std::string_view kPayload = "0123456789";

  EXPECT_EQ(engine_->ObjectKey(kKey), prefix_ + "/" + std::string(kKey));
  ASSERT_TRUE(Put(kKey, kPayload).ok());

  auto whole = folly::IOBuf::create(32);
  ASSERT_TRUE(Get(kKey, 0, 0, whole.get()).ok());
  EXPECT_EQ(std::string(reinterpret_cast<const char *>(whole->data()), whole->length()), kPayload);

  auto range = folly::IOBuf::create(4);
  ASSERT_TRUE(Get(kKey, 3, 4, range.get()).ok());
  EXPECT_EQ(std::string(reinterpret_cast<const char *>(range->data()), range->length()), "3456");

  auto suffix = folly::IOBuf::create(8);
  ASSERT_TRUE(Get(kKey, 6, 0, suffix.get()).ok());
  EXPECT_EQ(std::string(reinterpret_cast<const char *>(suffix->data()), suffix->length()), "6789");

  auto too_small = folly::IOBuf::create(3);
  EXPECT_EQ(Get(kKey, 0, 4, too_small.get()).ToErrno(), EINVAL);
  EXPECT_EQ(too_small->length(), 0);

  auto missing = folly::IOBuf::create(8);
  EXPECT_TRUE(Get("missing-object", 0, 0, missing.get()).IsNotFound());

  ASSERT_TRUE(Delete(kKey).ok());
  auto deleted = folly::IOBuf::create(8);
  EXPECT_TRUE(Get(kKey, 0, 0, deleted.get()).IsNotFound());
  EXPECT_TRUE(Delete(kKey).ok());
}

TEST_F(S3DataEngineE2ETest, TricklingResponseStillHonorsTerminalRequestDeadline) {
  TricklingS3Server server;
  DataEngineOptions options{
      .location = "s3://127.0.0.1:" + std::to_string(server.port()) + "/bucket",
      .region = "auto",
      .worker_count = 1,
      .request_timeout = 300ms,
      .retry_attempts = 0,
  };

  S3DataEngine engine(std::move(options));
  ASSERT_TRUE(engine.Initialize().ok());

  auto out = folly::IOBuf::create(128);
  Status status;
  const auto start = std::chrono::steady_clock::now();
  swordfs::test::RunInTestFiber([&] { status = engine.Get("trickling-object", 0, 128, out.get()); });
  const auto elapsed = std::chrono::steady_clock::now() - start;

  EXPECT_FALSE(status.ok()) << "a continuously progressing response must still have a finite terminal deadline";
  EXPECT_LT(elapsed, 2s);
}

}  // namespace
}  // namespace swordfs::storage
