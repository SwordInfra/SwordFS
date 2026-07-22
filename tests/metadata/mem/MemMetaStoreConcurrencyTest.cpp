// Copyright 2026 SwordFS Contributors.
// Licensed under the Apache License, Version 2.0.

// Concurrency tests for MemMetaStore.
// These tests validate the TOCTOU fix (PR #22) — after the fix, every
// public method holds mutex_ for its entire duration, making
// check-then-act sequences atomic.

#define FUSE_USE_VERSION 312
#include <fuse_lowlevel.h>
#include <gtest/gtest.h>
#include <sys/stat.h>

#include <atomic>
#include <barrier>
#include <chrono>
#include <set>
#include <thread>
#include <vector>

#include "metadata/mem/MemMetaStore.hpp"
#include "utils/Status.hpp"

using swordfs::metadata::InodeID;
using swordfs::metadata::MemMetaStore;
using swordfs::metadata::SwordFsInode;
using swordfs::utils::Status;

static constexpr InodeID kRoot = FUSE_ROOT_ID;
static constexpr mode_t kRegFile = S_IFREG | 0644;
static constexpr mode_t kDir = S_IFDIR | 0755;

// ════════════════════════════════════════════════════════════════════
// MemMetaStoreConcurrencyTest
// ════════════════════════════════════════════════════════════════════

class MemMetaStoreConcurrencyTest : public ::testing::Test {
 protected:
  void SetUp() override { store_ = new MemMetaStore(); }
  void TearDown() override { delete store_; }

  MemMetaStore* store_;
};

// ────────────────────────────────────────────────────────────────
// Concurrent AddEntry — no duplicate names under same parent
// ────────────────────────────────────────────────────────────────

TEST_F(MemMetaStoreConcurrencyTest, ConcurrentAddEntryNoDuplicate) {
  constexpr int kThreads = 8;
  constexpr int kPerThread = 50;  // total = 400 insertions

  std::atomic<int> error_count{0};
  std::atomic<int> success_count{0};

  auto worker = [&](int tid) {
    for (int i = 0; i < kPerThread; ++i) {
      // Each thread uses a unique name: "file_<tid>_<i>"
      std::string name = "file_" + std::to_string(tid) + "_" + std::to_string(i);
      SwordFsInode* child = nullptr;
      Status st = store_->AddEntry(kRoot, name, kRegFile, 0, &child);
      if (st.ok()) {
        success_count.fetch_add(1, std::memory_order_relaxed);
      } else if (st.IsAlreadyExists()) {
        error_count.fetch_add(1, std::memory_order_relaxed);
      } else {
        error_count.fetch_add(1, std::memory_order_relaxed);
      }
    }
  };

  std::vector<std::thread> threads;
  for (int t = 0; t < kThreads; ++t) {
    threads.emplace_back(worker, t);
  }
  for (auto& t : threads) t.join();

  EXPECT_EQ(success_count.load(), kThreads * kPerThread);
  EXPECT_EQ(error_count.load(), 0);
  // root + all created files
  EXPECT_EQ(store_->InodeCount(), 1 + kThreads * kPerThread);
}

// ────────────────────────────────────────────────────────────────
// Concurrent AddEntry — same name, only ONE should succeed
// ────────────────────────────────────────────────────────────────

TEST_F(MemMetaStoreConcurrencyTest, ConcurrentAddEntrySameName) {
  constexpr int kThreads = 16;

  std::atomic<int> success_count{0};
  std::atomic<int> exists_count{0};
  std::atomic<InodeID> winner_ino{0};

  // Use a barrier to start all threads at the same time — maximises
  // the chance of hitting the TOCTOU window if the lock is broken.
  std::barrier gate(kThreads);

  auto worker = [&]() {
    gate.arrive_and_wait();
    SwordFsInode* child = nullptr;
    Status st = store_->AddEntry(kRoot, "race_target", kRegFile, 0, &child);
    if (st.ok()) {
      success_count.fetch_add(1, std::memory_order_relaxed);
      winner_ino.store(child->ino, std::memory_order_relaxed);
    } else if (st.IsAlreadyExists()) {
      exists_count.fetch_add(1, std::memory_order_relaxed);
    }
  };

  std::vector<std::thread> threads;
  for (int t = 0; t < kThreads; ++t) {
    threads.emplace_back(worker);
  }
  for (auto& t : threads) t.join();

  // Exactly ONE thread must succeed.
  EXPECT_EQ(success_count.load(), 1)
      << "TOCTOU: more than one thread created 'race_target'";
  EXPECT_EQ(exists_count.load(), kThreads - 1);
  EXPECT_GT(winner_ino.load(), kRoot);
}

// ────────────────────────────────────────────────────────────────
// Concurrent MoveEntry — atomic check-then-act
// ────────────────────────────────────────────────────────────────

TEST_F(MemMetaStoreConcurrencyTest, ConcurrentMoveEntryAtomicity) {
  // Set up: root/src/file + root/dst/
  SwordFsInode* src_dir = nullptr;
  store_->AddEntry(kRoot, "src", kDir, 0, &src_dir);
  SwordFsInode* dst_dir = nullptr;
  store_->AddEntry(kRoot, "dst", kDir, 0, &dst_dir);
  SwordFsInode* f = nullptr;
  store_->AddEntry(src_dir->ino, "target", kRegFile, 0, &f);
  InodeID file_ino = f->ino;

  constexpr int kThreads = 8;
  std::atomic<int> moved_count{0};
  std::atomic<int> notfound_count{0};
  std::atomic<int> exists_count{0};

  std::barrier gate(kThreads);

  auto worker = [&](int tid) {
    gate.arrive_and_wait();
    // Each thread tries to move the same file to a unique destination name
    std::string new_name = "moved_" + std::to_string(tid);
    Status st = store_->MoveEntry(src_dir->ino, "target", dst_dir->ino, new_name);
    if (st.ok()) {
      moved_count.fetch_add(1, std::memory_order_relaxed);
    } else if (st.IsNotFound()) {
      notfound_count.fetch_add(1, std::memory_order_relaxed);
    } else if (st.IsAlreadyExists()) {
      exists_count.fetch_add(1, std::memory_order_relaxed);
    }
  };

  std::vector<std::thread> threads;
  for (int t = 0; t < kThreads; ++t) {
    threads.emplace_back(worker, t);
  }
  for (auto& t : threads) t.join();

  // Exactly ONE move should succeed (source is unlinked atomically).
  EXPECT_EQ(moved_count.load(), 1)
      << "TOCTOU: multiple threads moved the same source entry";
  EXPECT_EQ(notfound_count.load(), kThreads - 1);

  // Verify the file still exists with its original ino
  SwordFsInode* found = nullptr;
  for (int t = 0; t < kThreads; ++t) {
    std::string name = "moved_" + std::to_string(t);
    if (store_->LookupEntry(dst_dir->ino, name, &found).ok()) {
      EXPECT_EQ(found->ino, file_ino);
      break;
    }
  }
}

// ────────────────────────────────────────────────────────────────
// Concurrent RemoveEntry + AddEntry — no stale pointer use
// ────────────────────────────────────────────────────────────────

TEST_F(MemMetaStoreConcurrencyTest, ConcurrentRemoveAndAdd) {
  // Create 100 files under root
  constexpr int kFiles = 100;
  std::vector<InodeID> inodes;
  for (int i = 0; i < kFiles; ++i) {
    SwordFsInode* f = nullptr;
    store_->AddEntry(kRoot, "file_" + std::to_string(i), kRegFile, 0, &f);
    inodes.push_back(f->ino);
  }

  constexpr int kThreads = 4;
  std::atomic<int> ops_ok{0};
  std::atomic<int> ops_fail{0};

  auto remover = [&](int tid) {
    for (int i = tid * 25; i < (tid + 1) * 25 && i < kFiles; ++i) {
      Status st = store_->RemoveEntry(kRoot, "file_" + std::to_string(i));
      if (st.ok()) ops_ok.fetch_add(1, std::memory_order_relaxed);
      else ops_fail.fetch_add(1, std::memory_order_relaxed);
    }
  };

  auto adder = [&](int tid) {
    for (int i = 0; i < 25; ++i) {
      std::string name = "new_" + std::to_string(tid) + "_" + std::to_string(i);
      SwordFsInode* f = nullptr;
      Status st = store_->AddEntry(kRoot, name, kRegFile, 0, &f);
      if (st.ok()) ops_ok.fetch_add(1, std::memory_order_relaxed);
      else ops_fail.fetch_add(1, std::memory_order_relaxed);
    }
  };

  std::vector<std::thread> threads;
  // 4 removers + 4 adders
  for (int t = 0; t < kThreads; ++t) {
    threads.emplace_back(remover, t);
    threads.emplace_back(adder, t);
  }
  for (auto& t : threads) t.join();

  EXPECT_EQ(ops_fail.load(), 0);
  // After: root + 100 - 100 + 100 = 101 inodes
  EXPECT_EQ(store_->InodeCount(), 1 + kFiles) << "Inode count mismatch";
}

// ────────────────────────────────────────────────────────────────
// Concurrent AddEntry + ListEntries — snapshot consistency
// ────────────────────────────────────────────────────────────────

TEST_F(MemMetaStoreConcurrencyTest, ConcurrentAddAndList) {
  constexpr int kAdders = 4;
  constexpr int kListers = 2;
  constexpr int kFiles = 50;

  std::atomic<bool> start{false};
  std::atomic<int> lists_ok{0};
  std::atomic<int> lists_corrupt{0};

  auto adder = [&](int tid) {
    while (!start.load(std::memory_order_acquire)) { /* spin */ }
    for (int i = 0; i < kFiles; ++i) {
      std::string name = "item_" + std::to_string(tid) + "_" + std::to_string(i);
      store_->AddEntry(kRoot, name, kRegFile, 0, nullptr);
    }
  };

  auto lister = [&]() {
    while (!start.load(std::memory_order_acquire)) { /* spin */ }
    for (int round = 0; round < 20; ++round) {
      std::vector<std::pair<std::string, SwordFsInode*>> entries;
      Status st = store_->ListEntries(kRoot, &entries);
      if (st.ok()) {
        // Verify no duplicate names in the listing.
        std::set<std::string> names;
        bool duplicate = false;
        for (const auto& [name, _] : entries) {
          if (!names.insert(name).second) {
            duplicate = true;
            break;
          }
        }
        if (duplicate) {
          lists_corrupt.fetch_add(1, std::memory_order_relaxed);
        } else {
          lists_ok.fetch_add(1, std::memory_order_relaxed);
        }
      }
    }
  };

  std::vector<std::thread> threads;
  for (int t = 0; t < kAdders; ++t) threads.emplace_back(adder, t);
  for (int t = 0; t < kListers; ++t) threads.emplace_back(lister);

  start.store(true, std::memory_order_release);

  for (auto& t : threads) t.join();

  EXPECT_EQ(lists_corrupt.load(), 0)
      << "TOCTOU: ListEntries returned duplicate entries under concurrency";
  EXPECT_GT(lists_ok.load(), 0);
}

// ────────────────────────────────────────────────────────────────
// Concurrent move from multiple sources to same target
// ────────────────────────────────────────────────────────────────

TEST_F(MemMetaStoreConcurrencyTest, ConcurrentMoveToSameTarget) {
  // Set up: root/a/file1, root/b/file2, root/dst/
  SwordFsInode *dir_a = nullptr, *dir_b = nullptr, *dir_dst = nullptr;
  store_->AddEntry(kRoot, "a", kDir, 0, &dir_a);
  store_->AddEntry(kRoot, "b", kDir, 0, &dir_b);
  store_->AddEntry(kRoot, "dst", kDir, 0, &dir_dst);

  SwordFsInode *f1 = nullptr, *f2 = nullptr;
  store_->AddEntry(dir_a->ino, "file", kRegFile, 0, &f1);
  store_->AddEntry(dir_b->ino, "file", kRegFile, 0, &f2);

  std::atomic<int> moved{0};
  std::atomic<int> failed{0};
  std::barrier gate(2);

  auto mover_a = [&]() {
    gate.arrive_and_wait();
    Status st = store_->MoveEntry(dir_a->ino, "file", dir_dst->ino, "winner");
    if (st.ok()) moved.fetch_add(1, std::memory_order_relaxed);
    else failed.fetch_add(1, std::memory_order_relaxed);
  };

  auto mover_b = [&]() {
    gate.arrive_and_wait();
    Status st = store_->MoveEntry(dir_b->ino, "file", dir_dst->ino, "winner");
    if (st.ok()) moved.fetch_add(1, std::memory_order_relaxed);
    else failed.fetch_add(1, std::memory_order_relaxed);
  };

  std::thread t1(mover_a);
  std::thread t2(mover_b);
  t1.join();
  t2.join();

  EXPECT_EQ(moved.load(), 1)
      << "TOCTOU: two moves to the same target both succeeded";
  EXPECT_EQ(failed.load(), 1);

  // Verify exactly one file ended up at the target
  SwordFsInode* winner = nullptr;
  EXPECT_TRUE(store_->LookupEntry(dir_dst->ino, "winner", &winner).ok());
  bool winner_is_f1 = (winner->ino == f1->ino);
  bool winner_is_f2 = (winner->ino == f2->ino);
  EXPECT_TRUE(winner_is_f1 || winner_is_f2);
}
