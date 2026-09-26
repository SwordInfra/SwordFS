// Copyright 2026 SwordFS Contributors.
// Licensed under the Apache License, Version 2.0.

#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif

#include <fcntl.h>
#include <gtest/gtest.h>
#include <unistd.h>

#include <algorithm>
#include <array>
#include <cerrno>
#include <cstdlib>
#include <cstring>
#include <memory>
#include <string>
#include <thread>
#include <vector>

#include "tests/e2e/Fixture.hpp"

using swordfs::e2e::Fixture;

namespace {

class ScopedFd {
 public:
  explicit ScopedFd(int fd = -1) : fd_(fd) {
  }

  ~ScopedFd() {
    if (fd_ >= 0) {
      (void)::close(fd_);
    }
  }

  ScopedFd(const ScopedFd &) = delete;
  ScopedFd &operator=(const ScopedFd &) = delete;

  int get() const {
    return fd_;
  }

  void Reset(int fd = -1) {
    if (fd_ >= 0) {
      (void)::close(fd_);
    }
    fd_ = fd;
  }

 private:
  int fd_;
};

struct PipeReadResult {
  size_t bytes_read = 0;
  int error = 0;
  bool unexpected_eof = false;
  bool data_matches = true;
};

PipeReadResult ReadPipeBytes(int fd, size_t size) {
  PipeReadResult result;
  std::array<char, 64 * 1024> buffer{};

  while (result.bytes_read < size) {
    const size_t remaining = size - result.bytes_read;
    const size_t request = std::min(remaining, buffer.size());
    const ssize_t nread = ::read(fd, buffer.data(), request);
    if (nread < 0) {
      if (errno == EINTR) {
        continue;
      }
      result.error = errno;
      break;
    }
    if (nread == 0) {
      result.unexpected_eof = true;
      break;
    }

    result.data_matches = result.data_matches &&
                          std::all_of(buffer.begin(), buffer.begin() + nread, [](char value) { return value == 'x'; });
    result.bytes_read += static_cast<size_t>(nread);
  }

  return result;
}

::testing::AssertionResult RunSpliceCase(const Fixture &fixture, const std::string &name, bool direct,
                                         bool concurrent) {
  const long page_size = ::sysconf(_SC_PAGESIZE);
  if (page_size <= 0) {
    return ::testing::AssertionFailure() << "sysconf(_SC_PAGESIZE) failed";
  }

  const size_t alignment = static_cast<size_t>(page_size);
  const size_t payload_size = 150 * alignment;
  void *raw_buffer = nullptr;
  const int alloc_error = ::posix_memalign(&raw_buffer, alignment, payload_size);
  if (alloc_error != 0) {
    return ::testing::AssertionFailure() << "posix_memalign failed: " << std::strerror(alloc_error);
  }
  std::unique_ptr<void, decltype(&std::free)> payload(raw_buffer, &std::free);
  std::memset(payload.get(), 'x', payload_size);

  int open_flags = O_CREAT | O_TRUNC | O_RDWR;
  if (direct) {
    open_flags |= O_DIRECT;
  }
  ScopedFd file(::open(fixture.MountPath(name).c_str(), open_flags, 0644));
  if (file.get() < 0) {
    return ::testing::AssertionFailure() << "open failed: " << std::strerror(errno);
  }

  const ssize_t written = ::write(file.get(), payload.get(), payload_size);
  if (written != static_cast<ssize_t>(payload_size)) {
    return ::testing::AssertionFailure() << "write returned " << written << " expected " << payload_size << ": "
                                         << std::strerror(errno);
  }
  if (::lseek(file.get(), 0, SEEK_SET) != 0) {
    return ::testing::AssertionFailure() << "lseek failed: " << std::strerror(errno);
  }

  int pipe_fds[2];
  if (::pipe(pipe_fds) != 0) {
    return ::testing::AssertionFailure() << "pipe failed: " << std::strerror(errno);
  }
  ScopedFd pipe_read(pipe_fds[0]);
  ScopedFd pipe_write(pipe_fds[1]);

  PipeReadResult concurrent_read;
  std::thread reader;
  if (concurrent) {
    reader = std::thread([&] { concurrent_read = ReadPipeBytes(pipe_read.get(), payload_size); });
  }

  size_t bytes_spliced = 0;
  PipeReadResult sequential_read;
  int splice_error = 0;
  while (bytes_spliced < payload_size) {
    const size_t remaining = payload_size - bytes_spliced;
    const ssize_t spliced = ::splice(file.get(), nullptr, pipe_write.get(), nullptr, remaining, SPLICE_F_MOVE);
    if (spliced < 0) {
      if (errno == EINTR) {
        continue;
      }
      splice_error = errno;
      break;
    }
    if (spliced == 0) {
      splice_error = EIO;
      break;
    }

    bytes_spliced += static_cast<size_t>(spliced);
    if (!concurrent) {
      PipeReadResult batch = ReadPipeBytes(pipe_read.get(), static_cast<size_t>(spliced));
      sequential_read.bytes_read += batch.bytes_read;
      sequential_read.data_matches = sequential_read.data_matches && batch.data_matches;
      if (batch.error != 0 || batch.unexpected_eof) {
        sequential_read.error = batch.error;
        sequential_read.unexpected_eof = batch.unexpected_eof;
        break;
      }
    }
  }

  pipe_write.Reset();
  if (reader.joinable()) {
    reader.join();
  }

  const PipeReadResult &read_result = concurrent ? concurrent_read : sequential_read;
  if (splice_error != 0) {
    return ::testing::AssertionFailure() << "splice failed after " << bytes_spliced << "/" << payload_size
                                         << " bytes: " << std::strerror(splice_error);
  }
  if (bytes_spliced != payload_size) {
    return ::testing::AssertionFailure() << "splice transferred " << bytes_spliced << "/" << payload_size << " bytes";
  }
  if (read_result.error != 0) {
    return ::testing::AssertionFailure() << "pipe read failed after " << read_result.bytes_read << "/" << payload_size
                                         << " bytes: " << std::strerror(read_result.error);
  }
  if (read_result.unexpected_eof) {
    return ::testing::AssertionFailure() << "pipe reader hit EOF after " << read_result.bytes_read << "/"
                                         << payload_size << " bytes";
  }
  if (read_result.bytes_read != payload_size) {
    return ::testing::AssertionFailure() << "pipe reader consumed " << read_result.bytes_read << "/" << payload_size
                                         << " bytes";
  }
  if (!read_result.data_matches) {
    return ::testing::AssertionFailure() << "pipe data differs from the written payload";
  }

  return ::testing::AssertionSuccess();
}

}  // namespace

class SpliceReadTest : public ::testing::Test {
 protected:
  void SetUp() override {
    ASSERT_TRUE(fixture_.SetUp()) << "Failed to set up E2E fixture";
  }

  void TearDown() override {
    fixture_.TearDown();
  }

  Fixture fixture_;
};

TEST_F(SpliceReadTest, FileToPipeHandlesDirectAndBufferedReaders) {
  struct TestCase {
    const char *name;
    bool direct;
    bool concurrent;
  };
  constexpr std::array<TestCase, 4> kCases = {{{"concurrent-direct.bin", true, true},
                                               {"concurrent-buffered.bin", false, true},
                                               {"sequential-direct.bin", true, false},
                                               {"sequential-buffered.bin", false, false}}};

  for (const auto &test_case : kCases) {
    SCOPED_TRACE(test_case.name);
    EXPECT_TRUE(RunSpliceCase(fixture_, test_case.name, test_case.direct, test_case.concurrent));
  }
}
