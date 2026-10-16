// Copyright 2026 SwordFS Contributors.
// Licensed under the Apache License, Version 2.0.

#include <fcntl.h>
#include <folly/ScopeGuard.h>
#include <gtest/gtest.h>
#include <poll.h>
#include <signal.h>
#include <sw/redis++/redis++.h>
#include <sys/file.h>
#include <sys/syscall.h>
#include <sys/wait.h>
#include <unistd.h>

#include <cerrno>
#include <chrono>
#include <cstdlib>
#include <fstream>
#include <memory>
#include <stdexcept>
#include <string>
#include <thread>

#include "metadata/redis/RedisKey.hpp"
#include "metadata/redis/RedisMetaConfig.hpp"
#include "tests/e2e/Fixture.hpp"

namespace swordfs::e2e {
namespace {

using namespace std::chrono_literals;

constexpr auto kLeaseOptions =
    "--lock-lease-ttl 5s --lock-lease-renew-interval 1s "
    "--lock-lease-safety-margin 2s --lock-lease-io-timeout 250ms";

struct OwnerLeaseRedis {
  OwnerLeaseRedis(const char *url, const std::string &volume_name) : key(0, volume_name) {
    metadata::RedisMetaConfig config;
    const auto status = metadata::ParseRedisMetaUrl(url, &config);
    if (!status.ok()) {
      throw std::runtime_error(status.message());
    }
    key = metadata::redis::RedisKey(config.db, volume_name);
    sw::redis::ConnectionOptions options;
    options.host = config.host;
    options.port = config.port;
    options.db = config.db;
    if (config.username.has_value()) {
      options.user = *config.username;
    }
    if (config.password.has_value()) {
      options.password = *config.password;
    }
    redis = std::make_unique<sw::redis::Redis>(options);
  }

  metadata::redis::RedisKey key;
  std::unique_ptr<sw::redis::Redis> redis;
};

struct FlockWaiter {
  pid_t pid = -1;
  int read_fd = -1;
};

// A distinct process supplies an observable syscall result even if the
// blocked flock cannot be cancelled by a thread-local test timeout.
FlockWaiter StartFlockWaiter(int holder_fd, int peer_fd) {
  int ready_pipe[2];
  if (::pipe(ready_pipe) != 0) {
    return {};
  }
  const pid_t pid = ::fork();
  if (pid < 0) {
    ::close(ready_pipe[0]);
    ::close(ready_pipe[1]);
    return {};
  }
  if (pid == 0) {
    ::close(ready_pipe[0]);
    ::close(holder_fd);
    struct sigaction action{};
    action.sa_handler = +[](int) {};
    ::sigemptyset(&action.sa_mask);
    action.sa_flags = 0;
    ::sigaction(SIGUSR1, &action, nullptr);
    const char ready = 'R';
    (void)::write(ready_pipe[1], &ready, 1);
    const int result = ::flock(peer_fd, LOCK_EX);
    const char outcome = result == 0 ? 'G' : (errno == EINTR ? 'I' : 'E');
    (void)::write(ready_pipe[1], &outcome, 1);
    ::_exit(0);
  }
  ::close(ready_pipe[1]);
  return {pid, ready_pipe[0]};
}

// Like StartFlockWaiter, but the separate process issues a traditional
// blocking byte-range lock against the independent peer mount.
FlockWaiter StartPosixWaiter(int holder_fd, int peer_fd) {
  int ready_pipe[2];
  if (::pipe(ready_pipe) != 0) {
    return {};
  }
  const pid_t pid = ::fork();
  if (pid < 0) {
    ::close(ready_pipe[0]);
    ::close(ready_pipe[1]);
    return {};
  }
  if (pid == 0) {
    ::close(ready_pipe[0]);
    ::close(holder_fd);
    struct sigaction action{};
    action.sa_handler = +[](int) {};
    ::sigemptyset(&action.sa_mask);
    ::sigaction(SIGUSR1, &action, nullptr);
    struct flock lock{};
    lock.l_type = F_WRLCK;
    lock.l_whence = SEEK_SET;
    lock.l_start = 16;
    lock.l_len = 24;
    const char ready = 'R';
    (void)::write(ready_pipe[1], &ready, 1);
    const int result = ::fcntl(peer_fd, F_SETLKW, &lock);
    const char outcome = result == 0 ? 'G' : (errno == EINTR ? 'I' : 'E');
    (void)::write(ready_pipe[1], &outcome, 1);
    ::_exit(0);
  }
  ::close(ready_pipe[1]);
  return {pid, ready_pipe[0]};
}

char ReadWaiterEvent(int fd, int timeout_ms) {
  struct pollfd poll_fd{fd, POLLIN, 0};
  if (::poll(&poll_fd, 1, timeout_ms) != 1) {
    return '\0';
  }
  char event = '\0';
  return ::read(fd, &event, 1) == 1 ? event : '\0';
}

// The ready byte is sent before flock(2). Observe the child's actual Linux
// syscall and sleeping state before sending a signal or detaching FUSE;
// a pre-syscall signal would otherwise turn this into a timing-dependent test.
bool WaitForBlockedLockSyscall(pid_t pid, long syscall_number) {
  const std::string process_dir = "/proc/" + std::to_string(pid) + "/";
  for (int attempt = 0; attempt < 500; ++attempt) {
    std::ifstream syscall_file(process_dir + "syscall");
    long observed_syscall = -1;
    if (syscall_file >> observed_syscall && observed_syscall == syscall_number) {
      std::ifstream stat_file(process_dir + "stat");
      std::string process_stat;
      std::getline(stat_file, process_stat);
      // Comm may contain spaces or parentheses; the state follows its
      // closing ')' rather than an arbitrary whitespace-delimited column.
      const auto comm_end = process_stat.rfind(')');
      if (comm_end != std::string::npos && comm_end + 2 < process_stat.size() &&
          (process_stat[comm_end + 2] == 'S' || process_stat[comm_end + 2] == 'D')) {
        return true;
      }
    }
    std::this_thread::sleep_for(10ms);
  }
  return false;
}

TEST(OwnerLeaseE2ETest, DeletedLeaseDoesNotUnmountOrdinaryFuseFilesystem) {
  Fixture fixture;
  fixture.SetExtraMountOptions(kLeaseOptions);
  ASSERT_TRUE(fixture.SetUp());

  const char *url = std::getenv("SWORDFS_METADATA_URL");
  ASSERT_NE(url, nullptr);
  OwnerLeaseRedis authority(url, fixture.VolumeName());
  const auto current = authority.redis->get(authority.key.OwnerGeneration());
  ASSERT_TRUE(current.has_value());
  const auto generation = std::stoull(*current);
  ASSERT_GT(generation, 0U);
  ASSERT_GT(authority.redis->pttl(authority.key.OwnerLease(generation)), 0);

  // Server-side revocation is immediately authoritative. This test checks
  // ordinary FUSE availability while the lease key is absent; the native
  // Redis unit test separately verifies heartbeat detection and terminality.
  authority.redis->del(authority.key.OwnerLease(generation));
  ASSERT_EQ(authority.redis->exists(authority.key.OwnerLease(generation)), 0);
  struct stat st{};
  EXPECT_TRUE(fixture.IsMounted());
  EXPECT_EQ(fixture.Stat("", &st), 0);
  fixture.TearDown();
}

TEST(OwnerLeaseE2ETest, ExplicitUnmountDeletesOnlyItsExactOwnerLease) {
  Fixture fixture;
  fixture.SetExtraMountOptions(kLeaseOptions);
  ASSERT_TRUE(fixture.SetUp());

  const char *url = std::getenv("SWORDFS_METADATA_URL");
  ASSERT_NE(url, nullptr);
  OwnerLeaseRedis authority(url, fixture.VolumeName());
  const auto current = authority.redis->get(authority.key.OwnerGeneration());
  ASSERT_TRUE(current.has_value());
  const auto generation = std::stoull(*current);
  ASSERT_GT(generation, 0U);
  ASSERT_GT(authority.redis->pttl(authority.key.OwnerLease(generation)), 0);

  fixture.TearDown();
  EXPECT_EQ(authority.redis->exists(authority.key.OwnerLease(generation)), 0);
  EXPECT_EQ(authority.redis->get(authority.key.OwnerGeneration()), current);
}

TEST(OwnerLeaseE2ETest, InvalidLeaseTimingsDoNotPreventOrdinaryMount) {
  Fixture fixture;
  fixture.SetExtraMountOptions(
      "--lock-lease-ttl 5s --lock-lease-renew-interval 4s "
      "--lock-lease-safety-margin 2s --lock-lease-io-timeout 1s");
  ASSERT_TRUE(fixture.SetUp());

  const char *url = std::getenv("SWORDFS_METADATA_URL");
  ASSERT_NE(url, nullptr);
  OwnerLeaseRedis authority(url, fixture.VolumeName());
  EXPECT_EQ(authority.redis->get(authority.key.OwnerGeneration()), "0");
  struct stat st{};
  EXPECT_TRUE(fixture.IsMounted());
  EXPECT_EQ(fixture.Stat("", &st), 0);
  ASSERT_EQ(fixture.CreateFile("lock-unavailable", 0644, O_CREAT | O_RDWR), 0);
  const int fd = fixture.OpenFile("lock-unavailable", O_RDWR);
  ASSERT_GE(fd, 0);
  // This mount has no Redis owner generation; the FUSE callback must reject
  // flock, rather than allowing the kernel to silently grant a local lock.
  EXPECT_EQ(::flock(fd, LOCK_EX | LOCK_NB), -1);
  EXPECT_EQ(errno, EIO);
  struct flock record_lock{};
  record_lock.l_type = F_WRLCK;
  record_lock.l_whence = SEEK_SET;
  record_lock.l_start = 0;
  record_lock.l_len = 1;
  EXPECT_EQ(::fcntl(fd, F_SETLK, &record_lock), -1);
  EXPECT_EQ(errno, EIO) << "failed Redis registration must not fall back to a local POSIX lock";
  EXPECT_EQ(::close(fd), 0);
  EXPECT_EQ(fixture.WriteFile("lock-unavailable", "ordinary-data"), 0);
  std::string data;
  EXPECT_EQ(fixture.ReadFile("lock-unavailable", &data), 0);
  EXPECT_EQ(data, "ordinary-data");
  EXPECT_TRUE(fixture.IsMounted());
  fixture.TearDown();
}

TEST(OwnerLeaseE2ETest, CleanRemountAllocatesFreshGeneration) {
  Fixture fixture;
  fixture.SetExtraMountOptions(kLeaseOptions);
  ASSERT_TRUE(fixture.SetUp());

  const char *url = std::getenv("SWORDFS_METADATA_URL");
  ASSERT_NE(url, nullptr);
  OwnerLeaseRedis authority(url, fixture.VolumeName());
  const auto first = authority.redis->get(authority.key.OwnerGeneration());
  ASSERT_TRUE(first.has_value());
  const auto first_generation = std::stoull(*first);
  ASSERT_GT(authority.redis->pttl(authority.key.OwnerLease(first_generation)), 0);

  ASSERT_TRUE(fixture.Remount());
  const auto next = authority.redis->get(authority.key.OwnerGeneration());
  ASSERT_TRUE(next.has_value());
  const auto next_generation = std::stoull(*next);
  EXPECT_GT(next_generation, first_generation);
  EXPECT_EQ(authority.redis->exists(authority.key.OwnerLease(first_generation)), 0);
  EXPECT_GT(authority.redis->pttl(authority.key.OwnerLease(next_generation)), 0);
  fixture.TearDown();
}

TEST(OwnerLeaseE2ETest, IndependentKernelMountsEnforceRedisFlockConflictsAndLastClose) {
  Fixture fixture;
  ASSERT_TRUE(fixture.SetUp());
  auto cleanup = folly::makeGuard([&] { fixture.TearDown(); });
  ASSERT_EQ(fixture.CreateFile("shared", 0644, O_CREAT | O_RDWR), 0);
  ASSERT_TRUE(fixture.StartPeerMount());

  const int primary_fd = fixture.OpenFile("shared", O_RDWR);
  ASSERT_GE(primary_fd, 0);
  const int peer_fd = ::open(fixture.PeerMountPath("shared").c_str(), O_RDWR);
  ASSERT_GE(peer_fd, 0);

  struct stat inode_stat{};
  ASSERT_EQ(::fstat(primary_fd, &inode_stat), 0);
  OwnerLeaseRedis authority(std::getenv("SWORDFS_METADATA_URL"), fixture.VolumeName());

  ASSERT_EQ(::flock(primary_fd, LOCK_EX | LOCK_NB), 0);
  EXPECT_EQ(authority.redis->hlen(authority.key.FlockLock(inode_stat.st_ino)), 1);
  // A local-kernel-only flock would incorrectly succeed on this peer mount.
  EXPECT_EQ(::flock(peer_fd, LOCK_EX | LOCK_NB), -1);
  EXPECT_EQ(errno, EWOULDBLOCK);

  const int duplicated_fd = ::dup(primary_fd);
  ASSERT_GE(duplicated_fd, 0);
  EXPECT_EQ(::close(primary_fd), 0);
  EXPECT_EQ(::flock(peer_fd, LOCK_EX | LOCK_NB), -1);
  EXPECT_EQ(errno, EWOULDBLOCK);
  EXPECT_EQ(::close(duplicated_fd), 0);
  // RELEASE is asynchronous to close(2): wait for the observable Redis
  // unlock rather than assuming that close is a FUSE completion barrier.
  bool acquired = false;
  for (int attempt = 0; attempt < 100; ++attempt) {
    if (::flock(peer_fd, LOCK_EX | LOCK_NB) == 0) {
      acquired = true;
      break;
    }
    if (errno != EWOULDBLOCK) {
      break;
    }
    std::this_thread::sleep_for(20ms);
  }
  EXPECT_TRUE(acquired) << "last CLOSE must eventually release remote ownership";
  EXPECT_EQ(authority.redis->hlen(authority.key.FlockLock(inode_stat.st_ino)), 1);
  EXPECT_EQ(::flock(peer_fd, LOCK_UN), 0);
  EXPECT_EQ(::close(peer_fd), 0);
}

// The same POSIX byte range must conflict across independent FUSE mount
// sessions sharing one Redis volume. Before FUSE_CAP_POSIX_LOCKS is enabled,
// each kernel grants the lock locally, so this test supplies a real RED proof
// rather than accepting a passing local-kernel fcntl test as distributed.
TEST(OwnerLeaseE2ETest, IndependentKernelMountsEnforcePosixRecordLockConflicts) {
  Fixture fixture;
  ASSERT_TRUE(fixture.SetUp());
  auto cleanup = folly::makeGuard([&] { fixture.TearDown(); });
  ASSERT_EQ(fixture.CreateFile("record-lock", 0644, O_CREAT | O_RDWR), 0);
  ASSERT_TRUE(fixture.StartPeerMount());

  const int first_fd = fixture.OpenFile("record-lock", O_RDWR);
  ASSERT_GE(first_fd, 0);
  const int second_fd = ::open(fixture.PeerMountPath("record-lock").c_str(), O_RDWR);
  ASSERT_GE(second_fd, 0);
  auto descriptors = folly::makeGuard([&] {
    struct flock unlock{};
    unlock.l_type = F_UNLCK;
    unlock.l_whence = SEEK_SET;
    unlock.l_start = 16;
    unlock.l_len = 24;
    (void)::fcntl(second_fd, F_SETLK, &unlock);
    (void)::fcntl(first_fd, F_SETLK, &unlock);
    (void)::close(second_fd);
    (void)::close(first_fd);
  });

  struct flock exclusive{};
  exclusive.l_type = F_WRLCK;
  exclusive.l_whence = SEEK_SET;
  exclusive.l_start = 16;
  exclusive.l_len = 24;
  ASSERT_EQ(::fcntl(first_fd, F_SETLK, &exclusive), 0);

  struct flock conflict = exclusive;
  ASSERT_EQ(::fcntl(second_fd, F_GETLK, &conflict), 0);
  EXPECT_EQ(conflict.l_type, F_WRLCK) << "the peer must see the remote write lock";

  errno = 0;
  EXPECT_EQ(::fcntl(second_fd, F_SETLK, &exclusive), -1)
      << "the independent peer must not receive a conflicting Redis record lock";
  EXPECT_TRUE(errno == EAGAIN || errno == EACCES) << "unexpected POSIX conflict errno: " << errno;
}

TEST(OwnerLeaseE2ETest, PosixReadLocksAndAnyCloseUnlockAcrossMounts) {
  Fixture fixture;
  ASSERT_TRUE(fixture.SetUp());
  auto cleanup = folly::makeGuard([&] { fixture.TearDown(); });
  ASSERT_EQ(fixture.CreateFile("ranges", 0644, O_CREAT | O_RDWR), 0);
  ASSERT_TRUE(fixture.StartPeerMount());

  const int first_fd = fixture.OpenFile("ranges", O_RDWR);
  ASSERT_GE(first_fd, 0);
  int unrelated_fd = fixture.OpenFile("ranges", O_RDWR);
  ASSERT_GE(unrelated_fd, 0);
  const int peer_fd = ::open(fixture.PeerMountPath("ranges").c_str(), O_RDWR);
  ASSERT_GE(peer_fd, 0);
  auto descriptors = folly::makeGuard([&] {
    ::close(peer_fd);
    if (unrelated_fd >= 0) {
      ::close(unrelated_fd);
    }
    ::close(first_fd);
  });

  struct flock shared{};
  shared.l_type = F_RDLCK;
  shared.l_whence = SEEK_SET;
  shared.l_start = 16;
  shared.l_len = 24;
  ASSERT_EQ(::fcntl(first_fd, F_SETLK, &shared), 0);
  ASSERT_EQ(::fcntl(peer_fd, F_SETLK, &shared), 0) << "shared/read ranges must coexist across mounts";

  struct flock exclusive = shared;
  exclusive.l_type = F_WRLCK;
  errno = 0;
  EXPECT_EQ(::fcntl(peer_fd, F_SETLK, &exclusive), -1);
  EXPECT_TRUE(errno == EAGAIN || errno == EACCES);

  // Closing any unrelated descriptor for the same inode in the original
  // process must release all of that process's record locks. The same close
  // must not release a different mount's owner.
  ASSERT_EQ(::close(unrelated_fd), 0);
  unrelated_fd = -1;
  bool released = false;
  for (int attempt = 0; attempt < 100; ++attempt) {
    if (::fcntl(peer_fd, F_SETLK, &exclusive) == 0) {
      released = true;
      break;
    }
    if (errno != EAGAIN && errno != EACCES) {
      break;
    }
    std::this_thread::sleep_for(20ms);
  }
  EXPECT_TRUE(released) << "any-close must release the original process owner, not the peer owner";
  struct flock unlock = shared;
  unlock.l_type = F_UNLCK;
  EXPECT_EQ(::fcntl(peer_fd, F_SETLK, &unlock), 0);
}

TEST(OwnerLeaseE2ETest, BlockingPosixRecordLockWaitWakesOnPeerUnlock) {
  Fixture fixture;
  ASSERT_TRUE(fixture.SetUp());
  auto cleanup = folly::makeGuard([&] { fixture.TearDown(); });
  ASSERT_EQ(fixture.CreateFile("wait", 0644, O_CREAT | O_RDWR), 0);
  ASSERT_TRUE(fixture.StartPeerMount());
  const int holder_fd = fixture.OpenFile("wait", O_RDWR);
  ASSERT_GE(holder_fd, 0);
  const int peer_fd = ::open(fixture.PeerMountPath("wait").c_str(), O_RDWR);
  ASSERT_GE(peer_fd, 0);
  struct flock lock{};
  lock.l_type = F_WRLCK;
  lock.l_whence = SEEK_SET;
  lock.l_start = 16;
  lock.l_len = 24;
  ASSERT_EQ(::fcntl(holder_fd, F_SETLK, &lock), 0);
  const auto waiter = StartPosixWaiter(holder_fd, peer_fd);
  ASSERT_GT(waiter.pid, 0);
  auto waiter_cleanup = folly::makeGuard([&] {
    (void)::kill(waiter.pid, SIGKILL);
    (void)::waitpid(waiter.pid, nullptr, 0);
    ::close(waiter.read_fd);
    ::close(peer_fd);
    ::close(holder_fd);
  });
  ASSERT_EQ(ReadWaiterEvent(waiter.read_fd, 2000), 'R');
  ASSERT_TRUE(WaitForBlockedLockSyscall(waiter.pid, SYS_fcntl));
  EXPECT_EQ(ReadWaiterEvent(waiter.read_fd, 250), '\0');
  lock.l_type = F_UNLCK;
  ASSERT_EQ(::fcntl(holder_fd, F_SETLK, &lock), 0);
  EXPECT_EQ(ReadWaiterEvent(waiter.read_fd, 6000), 'G');
}

TEST(OwnerLeaseE2ETest, SignalInterruptCancelsBlockingPosixRecordLock) {
  Fixture fixture;
  ASSERT_TRUE(fixture.SetUp());
  auto cleanup = folly::makeGuard([&] { fixture.TearDown(); });
  ASSERT_EQ(fixture.CreateFile("interrupt", 0644, O_CREAT | O_RDWR), 0);
  ASSERT_TRUE(fixture.StartPeerMount());
  const int holder_fd = fixture.OpenFile("interrupt", O_RDWR);
  ASSERT_GE(holder_fd, 0);
  const int peer_fd = ::open(fixture.PeerMountPath("interrupt").c_str(), O_RDWR);
  ASSERT_GE(peer_fd, 0);
  struct flock lock{};
  lock.l_type = F_WRLCK;
  lock.l_whence = SEEK_SET;
  lock.l_start = 16;
  lock.l_len = 24;
  ASSERT_EQ(::fcntl(holder_fd, F_SETLK, &lock), 0);
  const auto waiter = StartPosixWaiter(holder_fd, peer_fd);
  ASSERT_GT(waiter.pid, 0);
  auto waiter_cleanup = folly::makeGuard([&] {
    (void)::kill(waiter.pid, SIGKILL);
    (void)::waitpid(waiter.pid, nullptr, 0);
    ::close(waiter.read_fd);
    ::close(peer_fd);
    ::close(holder_fd);
  });
  ASSERT_EQ(ReadWaiterEvent(waiter.read_fd, 2000), 'R');
  ASSERT_TRUE(WaitForBlockedLockSyscall(waiter.pid, SYS_fcntl));
  ASSERT_EQ(::kill(waiter.pid, SIGUSR1), 0);
  EXPECT_EQ(ReadWaiterEvent(waiter.read_fd, 6000), 'I');
  lock.l_type = F_UNLCK;
  ASSERT_EQ(::fcntl(holder_fd, F_SETLK, &lock), 0);
  const int independent_fd = ::open(fixture.PeerMountPath("interrupt").c_str(), O_RDWR);
  ASSERT_GE(independent_fd, 0);
  lock.l_type = F_WRLCK;
  EXPECT_EQ(::fcntl(independent_fd, F_SETLK, &lock), 0);
  lock.l_type = F_UNLCK;
  EXPECT_EQ(::fcntl(independent_fd, F_SETLK, &lock), 0);
  EXPECT_EQ(::close(independent_fd), 0);
}

TEST(OwnerLeaseE2ETest, IndependentMountsShareSharedFlockAndConvertWithoutLocalFallback) {
  Fixture fixture;
  ASSERT_TRUE(fixture.SetUp());
  auto cleanup = folly::makeGuard([&] { fixture.TearDown(); });
  ASSERT_EQ(fixture.CreateFile("shared", 0644, O_CREAT | O_RDWR), 0);
  ASSERT_TRUE(fixture.StartPeerMount());
  const int first_fd = fixture.OpenFile("shared", O_RDWR);
  ASSERT_GE(first_fd, 0);
  const int second_fd = ::open(fixture.PeerMountPath("shared").c_str(), O_RDWR);
  ASSERT_GE(second_fd, 0);

  ASSERT_EQ(::flock(first_fd, LOCK_SH | LOCK_NB), 0);
  ASSERT_EQ(::flock(second_fd, LOCK_SH | LOCK_NB), 0);
  EXPECT_EQ(::flock(second_fd, LOCK_EX | LOCK_NB), -1);
  EXPECT_EQ(errno, EWOULDBLOCK);
  EXPECT_EQ(::flock(second_fd, LOCK_UN), 0);

  // After releasing the competing SH, upgrade and downgrade the surviving
  // owner without changing its open-description identity.
  EXPECT_EQ(::flock(first_fd, LOCK_EX | LOCK_NB), 0);
  EXPECT_EQ(::flock(second_fd, LOCK_SH | LOCK_NB), -1);
  EXPECT_EQ(errno, EWOULDBLOCK);
  EXPECT_EQ(::flock(first_fd, LOCK_SH | LOCK_NB), 0);
  EXPECT_EQ(::flock(second_fd, LOCK_SH | LOCK_NB), 0);
  EXPECT_EQ(::flock(second_fd, LOCK_UN), 0);
  EXPECT_EQ(::flock(first_fd, LOCK_UN), 0);
  EXPECT_EQ(::close(second_fd), 0);
  EXPECT_EQ(::close(first_fd), 0);
}

TEST(OwnerLeaseE2ETest, ForkInheritedFlockSurvivesParentCloseUntilChildLastClose) {
  Fixture fixture;
  ASSERT_TRUE(fixture.SetUp());
  auto cleanup = folly::makeGuard([&] { fixture.TearDown(); });
  ASSERT_EQ(fixture.CreateFile("shared", 0644, O_CREAT | O_RDWR), 0);
  ASSERT_TRUE(fixture.StartPeerMount());

  int holder_fd = fixture.OpenFile("shared", O_RDWR);
  ASSERT_GE(holder_fd, 0);
  const int peer_fd = ::open(fixture.PeerMountPath("shared").c_str(), O_RDWR);
  ASSERT_GE(peer_fd, 0);
  auto descriptor_cleanup = folly::makeGuard([&] {
    if (holder_fd >= 0) {
      ::close(holder_fd);
    }
    ::close(peer_fd);
  });
  ASSERT_EQ(::flock(holder_fd, LOCK_EX), 0);

  int control[2];
  int events[2];
  ASSERT_EQ(::pipe(control), 0);
  ASSERT_EQ(::pipe(events), 0);
  pid_t child_pid = ::fork();
  ASSERT_GE(child_pid, 0);
  if (child_pid == 0) {
    ::close(control[1]);
    ::close(events[0]);
    ::close(peer_fd);
    const char ready = 'R';
    (void)::write(events[1], &ready, 1);
    char command = '\0';
    if (::read(control[0], &command, 1) == 1 && command == 'C') {
      ::close(holder_fd);
      const char closed = 'C';
      (void)::write(events[1], &closed, 1);
    }
    ::_exit(0);
  }
  ::close(control[0]);
  ::close(events[1]);
  auto child_cleanup = folly::makeGuard([&] {
    if (child_pid > 0) {
      (void)::kill(child_pid, SIGKILL);
      (void)::waitpid(child_pid, nullptr, 0);
    }
    ::close(control[1]);
    ::close(events[0]);
  });
  ASSERT_EQ(ReadWaiterEvent(events[0], 2000), 'R');
  ASSERT_EQ(::close(holder_fd), 0);
  holder_fd = -1;
  EXPECT_EQ(::flock(peer_fd, LOCK_EX | LOCK_NB), -1);
  EXPECT_EQ(errno, EWOULDBLOCK) << "fork-inherited description must retain its remote flock";
  const char close_child = 'C';
  ASSERT_EQ(::write(control[1], &close_child, 1), 1);
  ASSERT_EQ(ReadWaiterEvent(events[0], 2000), 'C');
  int status = 0;
  ASSERT_EQ(::waitpid(child_pid, &status, 0), child_pid);
  child_pid = -1;
  ASSERT_TRUE(WIFEXITED(status));
  EXPECT_EQ(WEXITSTATUS(status), 0);
  // FUSE RELEASE may complete after close(2); observe the actual peer grant.
  bool acquired = false;
  for (int attempt = 0; attempt < 100; ++attempt) {
    if (::flock(peer_fd, LOCK_EX | LOCK_NB) == 0) {
      acquired = true;
      break;
    }
    if (errno != EWOULDBLOCK) {
      break;
    }
    std::this_thread::sleep_for(20ms);
  }
  EXPECT_TRUE(acquired) << "last inherited close must release remote ownership";
  if (acquired) {
    EXPECT_EQ(::flock(peer_fd, LOCK_UN), 0);
  }
}

TEST(OwnerLeaseE2ETest, BlockingFlockOnPeerMountWakesOnRemoteUnlock) {
  Fixture fixture;
  ASSERT_TRUE(fixture.SetUp());
  auto cleanup = folly::makeGuard([&] { fixture.TearDown(); });
  ASSERT_EQ(fixture.CreateFile("shared", 0644, O_CREAT | O_RDWR), 0);
  ASSERT_TRUE(fixture.StartPeerMount());
  const int holder_fd = fixture.OpenFile("shared", O_RDWR);
  ASSERT_GE(holder_fd, 0);
  const int peer_fd = ::open(fixture.PeerMountPath("shared").c_str(), O_RDWR);
  ASSERT_GE(peer_fd, 0);
  ASSERT_EQ(::flock(holder_fd, LOCK_EX), 0);

  const auto waiter = StartFlockWaiter(holder_fd, peer_fd);
  ASSERT_GT(waiter.pid, 0);
  auto waiter_cleanup = folly::makeGuard([&] {
    (void)::kill(waiter.pid, SIGKILL);
    (void)::waitpid(waiter.pid, nullptr, 0);
    ::close(waiter.read_fd);
    ::close(peer_fd);
    ::close(holder_fd);
  });
  ASSERT_EQ(ReadWaiterEvent(waiter.read_fd, 2000), 'R');
  ASSERT_TRUE(WaitForBlockedLockSyscall(waiter.pid, SYS_flock));
  EXPECT_EQ(ReadWaiterEvent(waiter.read_fd, 250), '\0') << "peer waiter must not bypass the remote EX owner";
  ASSERT_EQ(::flock(holder_fd, LOCK_UN), 0);
  EXPECT_EQ(ReadWaiterEvent(waiter.read_fd, 6000), 'G') << "blocked peer must wake after remote unlock";
}

TEST(OwnerLeaseE2ETest, SignalInterruptOnPeerMountDoesNotAcquireAfterCancellation) {
  Fixture fixture;
  ASSERT_TRUE(fixture.SetUp());
  auto cleanup = folly::makeGuard([&] { fixture.TearDown(); });
  ASSERT_EQ(fixture.CreateFile("shared", 0644, O_CREAT | O_RDWR), 0);
  ASSERT_TRUE(fixture.StartPeerMount());
  const int holder_fd = fixture.OpenFile("shared", O_RDWR);
  ASSERT_GE(holder_fd, 0);
  const int peer_fd = ::open(fixture.PeerMountPath("shared").c_str(), O_RDWR);
  ASSERT_GE(peer_fd, 0);
  ASSERT_EQ(::flock(holder_fd, LOCK_EX), 0);

  const auto waiter = StartFlockWaiter(holder_fd, peer_fd);
  ASSERT_GT(waiter.pid, 0);
  auto waiter_cleanup = folly::makeGuard([&] {
    (void)::kill(waiter.pid, SIGKILL);
    (void)::waitpid(waiter.pid, nullptr, 0);
    ::close(waiter.read_fd);
    ::close(peer_fd);
    ::close(holder_fd);
  });
  ASSERT_EQ(ReadWaiterEvent(waiter.read_fd, 2000), 'R');
  ASSERT_TRUE(WaitForBlockedLockSyscall(waiter.pid, SYS_flock));
  ASSERT_EQ(ReadWaiterEvent(waiter.read_fd, 250), '\0');
  ASSERT_EQ(::kill(waiter.pid, SIGUSR1), 0);
  EXPECT_EQ(ReadWaiterEvent(waiter.read_fd, 6000), 'I') << "FUSE wait interruption must return EINTR";
  ASSERT_EQ(::flock(holder_fd, LOCK_UN), 0);

  const int independent_fd = ::open(fixture.PeerMountPath("shared").c_str(), O_RDWR);
  ASSERT_GE(independent_fd, 0);
  EXPECT_EQ(::flock(independent_fd, LOCK_EX | LOCK_NB), 0);
  EXPECT_EQ(::flock(independent_fd, LOCK_UN), 0);
  EXPECT_EQ(::close(independent_fd), 0);
}

TEST(OwnerLeaseE2ETest, LazyUnmountWithPendingPeerFlockExitsAfterDescriptionsClose) {
  Fixture fixture;
  ASSERT_TRUE(fixture.SetUp());
  auto cleanup = folly::makeGuard([&] { fixture.TearDown(); });
  ASSERT_EQ(fixture.CreateFile("shared", 0644, O_CREAT | O_RDWR), 0);
  ASSERT_TRUE(fixture.StartPeerMount());
  const char *url = std::getenv("SWORDFS_METADATA_URL");
  ASSERT_NE(url, nullptr);
  OwnerLeaseRedis authority(url, fixture.VolumeName());
  const auto peer_generation = authority.redis->get(authority.key.OwnerGeneration());
  ASSERT_TRUE(peer_generation.has_value());
  const auto peer_lease_key = authority.key.OwnerLease(std::stoull(*peer_generation));
  ASSERT_GT(authority.redis->pttl(peer_lease_key), 0);
  const int holder_fd = fixture.OpenFile("shared", O_RDWR);
  ASSERT_GE(holder_fd, 0);
  int peer_fd = ::open(fixture.PeerMountPath("shared").c_str(), O_RDWR);
  ASSERT_GE(peer_fd, 0);
  ASSERT_EQ(::flock(holder_fd, LOCK_EX), 0);

  auto waiter = StartFlockWaiter(holder_fd, peer_fd);
  ASSERT_GT(waiter.pid, 0);
  auto waiter_cleanup = folly::makeGuard([&] {
    if (waiter.pid > 0) {
      (void)::kill(waiter.pid, SIGKILL);
      (void)::waitpid(waiter.pid, nullptr, 0);
    }
    ::close(waiter.read_fd);
    if (peer_fd >= 0) {
      ::close(peer_fd);
    }
    ::close(holder_fd);
  });
  ASSERT_EQ(ReadWaiterEvent(waiter.read_fd, 2000), 'R');
  ASSERT_TRUE(WaitForBlockedLockSyscall(waiter.pid, SYS_flock));
  ASSERT_EQ(ReadWaiterEvent(waiter.read_fd, 250), '\0');

  // Ordinary unmount is EBUSY with open file descriptions. A user-requested
  // lazy unmount detaches the mountpoint while the blocked syscall and open
  // descriptions still hold the FUSE connection. It cannot require an
  // immediate daemon exit or cancel applications' outstanding syscalls.
  ASSERT_TRUE(fixture.DetachPeerMount());
  EXPECT_EQ(ReadWaiterEvent(waiter.read_fd, 250), '\0') << "detaching must not grant a conflicting lock";
  ASSERT_EQ(::kill(waiter.pid, SIGUSR1), 0);
  ASSERT_EQ(ReadWaiterEvent(waiter.read_fd, 2000), 'I');
  // Let the child's interrupt close its description, then close the other
  // open description so the detached FUSE session can exit normally.
  int status = 0;
  ASSERT_EQ(::waitpid(waiter.pid, &status, 0), waiter.pid);
  waiter.pid = 0;
  ASSERT_TRUE(WIFEXITED(status));
  EXPECT_EQ(WEXITSTATUS(status), 0);
  ASSERT_EQ(::close(peer_fd), 0);
  peer_fd = -1;
  EXPECT_TRUE(fixture.StopPeerMount()) << "daemon should exit after all detached peer file descriptions close";
  EXPECT_EQ(authority.redis->exists(peer_lease_key), 0)
      << "detached peer must revoke its precise Redis lease after daemon exit";
  struct stat st{};
  EXPECT_EQ(fixture.Stat("shared", &st), 0);
  EXPECT_EQ(::flock(holder_fd, LOCK_UN), 0);
}

TEST(OwnerLeaseE2ETest, RevokedDefaultOwnerFailsClosedWithoutUnmountingDataService) {
  Fixture fixture;
  ASSERT_TRUE(fixture.SetUp());
  auto cleanup = folly::makeGuard([&] { fixture.TearDown(); });
  ASSERT_EQ(fixture.CreateFile("ordinary", 0644, O_CREAT | O_RDWR), 0);
  const char *url = std::getenv("SWORDFS_METADATA_URL");
  ASSERT_NE(url, nullptr);
  OwnerLeaseRedis authority(url, fixture.VolumeName());
  const auto generation = authority.redis->get(authority.key.OwnerGeneration());
  ASSERT_TRUE(generation.has_value());
  const auto lease_key = authority.key.OwnerLease(std::stoull(*generation));
  ASSERT_GT(authority.redis->pttl(lease_key), 0);
  authority.redis->del(lease_key);

  const int fd = fixture.OpenFile("ordinary", O_RDWR);
  ASSERT_GE(fd, 0);
  EXPECT_EQ(::flock(fd, LOCK_EX | LOCK_NB), -1);
  EXPECT_NE(errno, EWOULDBLOCK) << "a lost lease must not be reported as ordinary contention";
  EXPECT_EQ(::close(fd), 0);
  struct stat st{};
  EXPECT_EQ(fixture.Stat("ordinary", &st), 0);
  EXPECT_EQ(fixture.WriteFile("ordinary", "ordinary-data"), 0);
  std::string data;
  EXPECT_EQ(fixture.ReadFile("ordinary", &data), 0);
  EXPECT_EQ(data, "ordinary-data");
  EXPECT_TRUE(fixture.IsMounted());
}

TEST(OwnerLeaseE2ETest, DefaultMountsRegisterDistinctLiveRedisOwners) {
  Fixture fixture;
  ASSERT_TRUE(fixture.SetUp());
  auto cleanup = folly::makeGuard([&] { fixture.TearDown(); });
  const char *url = std::getenv("SWORDFS_METADATA_URL");
  ASSERT_NE(url, nullptr);
  OwnerLeaseRedis authority(url, fixture.VolumeName());
  const auto first = authority.redis->get(authority.key.OwnerGeneration());
  ASSERT_TRUE(first.has_value());
  const uint64_t first_generation = std::stoull(*first);
  ASSERT_GT(first_generation, 0U);
  EXPECT_GT(authority.redis->pttl(authority.key.OwnerLease(first_generation)), 0);

  ASSERT_TRUE(fixture.StartPeerMount());
  const auto second = authority.redis->get(authority.key.OwnerGeneration());
  ASSERT_TRUE(second.has_value());
  const uint64_t second_generation = std::stoull(*second);
  EXPECT_GT(second_generation, first_generation);
  EXPECT_GT(authority.redis->pttl(authority.key.OwnerLease(first_generation)), 0);
  EXPECT_GT(authority.redis->pttl(authority.key.OwnerLease(second_generation)), 0);
  EXPECT_TRUE(fixture.StopPeerMount());
  EXPECT_EQ(authority.redis->exists(authority.key.OwnerLease(second_generation)), 0);
}

}  // namespace
}  // namespace swordfs::e2e
