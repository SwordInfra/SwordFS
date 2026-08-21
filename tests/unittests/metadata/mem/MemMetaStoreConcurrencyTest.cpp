// Copyright 2026 SwordFS Contributors.
// Licensed under the Apache License, Version 2.0.

// Concurrency tests for MemMetaStore.
// These tests validate the TOCTOU fix (PR #22) — after the fix, every
// operation runs as a single transaction (mutex_ held for its whole
// duration), making check-then-act sequences atomic.  Like the
// production code, the tests go through Transact() exclusively.

#include <gtest/gtest.h>
#include <sys/stat.h>

#include <atomic>
#include <barrier>
#include <set>
#include <string>
#include <thread>
#include <vector>

#include "metadata/mem/MemMetaStore.hpp"
#include "utils/Status.hpp"

using swordfs::metadata::InodeID;
using swordfs::metadata::MemMetaStore;
using swordfs::metadata::MemMetaTxn;
using swordfs::metadata::SwordFsEntry;
using swordfs::metadata::SwordFsInode;
using swordfs::utils::Status;

static constexpr InodeID kRoot = swordfs::metadata::kRootInodeId;
static constexpr mode_t kRegFile = S_IFREG | 0644;
static constexpr mode_t kDir = S_IFDIR | 0755;

// ════════════════════════════════════════════════════════════════════
// MemMetaStoreConcurrencyTest
// ════════════════════════════════════════════════════════════════════

class MemMetaStoreConcurrencyTest : public ::testing::Test {
 protected:
  void SetUp() override { store_ = new MemMetaStore(); }
  void TearDown() override { delete store_; }

  MemMetaStore *store_;
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
      Status status = store_->Transact([&](MemMetaTxn &txn) {
        return txn.AddEntry(kRoot, name, kRegFile, nullptr);
      });
      if (status.ok()) {
        success_count.fetch_add(1, std::memory_order_relaxed);
      } else {
        error_count.fetch_add(1, std::memory_order_relaxed);
      }
    }
  };

  std::vector<std::thread> threads;
  for (int t = 0; t < kThreads; ++t) {
    threads.emplace_back(worker, t);
  }
  for (auto &t : threads) {
    t.join();
  }

  EXPECT_EQ(success_count.load(), kThreads * kPerThread);
  EXPECT_EQ(error_count.load(), 0);
  // root + all created files
  size_t inode_count = store_->Transact(
      [&](MemMetaTxn &txn) { return txn.InodeCount(); });
  EXPECT_EQ(inode_count, 1 + kThreads * kPerThread);
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
    SwordFsInode child;
    Status status = store_->Transact([&](MemMetaTxn &txn) {
      return txn.AddEntry(kRoot, "race_target", kRegFile, &child);
    });
    if (status.ok()) {
      success_count.fetch_add(1, std::memory_order_relaxed);
      winner_ino.store(child.ino, std::memory_order_relaxed);
    } else if (status.IsAlreadyExists()) {
      exists_count.fetch_add(1, std::memory_order_relaxed);
    }
  };

  std::vector<std::thread> threads;
  for (int t = 0; t < kThreads; ++t) {
    threads.emplace_back(worker);
  }
  for (auto &t : threads) {
    t.join();
  }

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
  SwordFsInode src_dir;
  store_->Transact([&](MemMetaTxn &txn) {
    return txn.AddEntry(kRoot, "src", kDir, &src_dir);
  });
  SwordFsInode dst_dir;
  store_->Transact([&](MemMetaTxn &txn) {
    return txn.AddEntry(kRoot, "dst", kDir, &dst_dir);
  });
  SwordFsInode f;
  store_->Transact([&](MemMetaTxn &txn) {
    return txn.AddEntry(src_dir.ino, "target", kRegFile, &f);
  });
  InodeID file_ino = f.ino;

  constexpr int kThreads = 8;
  std::atomic<int> moved_count{0};
  std::atomic<int> notfound_count{0};
  std::atomic<int> exists_count{0};

  std::barrier gate(kThreads);

  auto worker = [&](int tid) {
    gate.arrive_and_wait();
    // Each thread tries to move the same file to a unique destination name
    std::string new_name = "moved_" + std::to_string(tid);
    Status status = store_->Transact([&](MemMetaTxn &txn) {
      return txn.MoveEntry(src_dir.ino, "target", dst_dir.ino, new_name, false);
    });
    if (status.ok()) {
      moved_count.fetch_add(1, std::memory_order_relaxed);
    } else if (status.IsNotFound()) {
      notfound_count.fetch_add(1, std::memory_order_relaxed);
    } else if (status.IsAlreadyExists()) {
      exists_count.fetch_add(1, std::memory_order_relaxed);
    }
  };

  std::vector<std::thread> threads;
  for (int t = 0; t < kThreads; ++t) {
    threads.emplace_back(worker, t);
  }
  for (auto &t : threads) {
    t.join();
  }

  // Exactly ONE move should succeed (source is unlinked atomically).
  EXPECT_EQ(moved_count.load(), 1)
      << "TOCTOU: multiple threads moved the same source entry";
  EXPECT_EQ(notfound_count.load(), kThreads - 1);

  // Verify the file still exists with its original ino
  SwordFsInode found;
  for (int t = 0; t < kThreads; ++t) {
    std::string name = "moved_" + std::to_string(t);
    Status status = store_->Transact([&](MemMetaTxn &txn) {
      return txn.LookupEntry(dst_dir.ino, name, &found);
    });
    if (status.ok()) {
      EXPECT_EQ(found.ino, file_ino);
      break;
    }
  }
}

// ────────────────────────────────────────────────────────────────
// Concurrent Unlink + AddEntry — no stale snapshot use
// ────────────────────────────────────────────────────────────────

TEST_F(MemMetaStoreConcurrencyTest, ConcurrentRemoveAndAdd) {
  // Create 100 files under root
  constexpr int kFiles = 100;
  std::vector<InodeID> inodes;
  for (int i = 0; i < kFiles; ++i) {
    SwordFsInode f;
    store_->Transact([&](MemMetaTxn &txn) {
      return txn.AddEntry(kRoot, "file_" + std::to_string(i), kRegFile, &f);
    });
    inodes.push_back(f.ino);
  }

  constexpr int kThreads = 4;
  std::atomic<int> ops_ok{0};
  std::atomic<int> ops_fail{0};

  auto remover = [&](int tid) {
    for (int i = tid * 25; i < (tid + 1) * 25 && i < kFiles; ++i) {
      // Unlink + ReclaimInode in one transaction — this mirrors what
      // VfsImpl::Unlink does for a no-fd case.
      InodeID target_ino = inodes[i];
      std::string name = "file_" + std::to_string(i);
      Status status = store_->Transact([&](MemMetaTxn &txn) {
        Status status = txn.Unlink(kRoot, name);
        if (!status.ok()) {
          return status;
        }
        return txn.ReclaimInode(target_ino);
      });
      if (status.ok()) {
        ops_ok.fetch_add(1, std::memory_order_relaxed);
      } else {
        ops_fail.fetch_add(1, std::memory_order_relaxed);
      }
    }
  };

  auto adder = [&](int tid) {
    for (int i = 0; i < 25; ++i) {
      std::string name = "new_" + std::to_string(tid) + "_" + std::to_string(i);
      Status status = store_->Transact([&](MemMetaTxn &txn) {
        return txn.AddEntry(kRoot, name, kRegFile, nullptr);
      });
      if (status.ok()) {
        ops_ok.fetch_add(1, std::memory_order_relaxed);
      } else {
        ops_fail.fetch_add(1, std::memory_order_relaxed);
      }
    }
  };

  std::vector<std::thread> threads;
  // 4 removers + 4 adders
  for (int t = 0; t < kThreads; ++t) {
    threads.emplace_back(remover, t);
    threads.emplace_back(adder, t);
  }
  for (auto &t : threads) {
    t.join();
  }

  EXPECT_EQ(ops_fail.load(), 0);
  // After: root + 100 - 100 + 100 = 101 inodes
  size_t inode_count = store_->Transact(
      [&](MemMetaTxn &txn) { return txn.InodeCount(); });
  EXPECT_EQ(inode_count, 1 + kFiles) << "Inode count mismatch";
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
    while (!start.load(std::memory_order_acquire)) { /* spin */
    }
    for (int i = 0; i < kFiles; ++i) {
      std::string name = "item_" + std::to_string(tid) + "_" + std::to_string(i);
      store_->Transact([&](MemMetaTxn &txn) {
        return txn.AddEntry(kRoot, name, kRegFile, nullptr);
      });
    }
  };

  auto lister = [&]() {
    while (!start.load(std::memory_order_acquire)) { /* spin */
    }
    for (int round = 0; round < 20; ++round) {
      std::vector<SwordFsEntry> entries;
      Status status = store_->Transact([&](MemMetaTxn &txn) {
        return txn.ListEntries(kRoot, &entries);
      });
      if (status.ok()) {
        // Verify no duplicate names in the listing.
        std::set<std::string> names;
        bool duplicate = false;
        for (const auto &entry : entries) {
          if (!names.insert(entry.name).second) {
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
  for (int t = 0; t < kAdders; ++t) {
    threads.emplace_back(adder, t);
  }
  for (int t = 0; t < kListers; ++t) {
    threads.emplace_back(lister);
  }

  start.store(true, std::memory_order_release);

  for (auto &t : threads) {
    t.join();
  }

  EXPECT_EQ(lists_corrupt.load(), 0)
      << "TOCTOU: ListEntries returned duplicate entries under concurrency";
  EXPECT_GT(lists_ok.load(), 0);
}

// ────────────────────────────────────────────────────────────────
// Concurrent move from multiple sources to same target
// ────────────────────────────────────────────────────────────────

TEST_F(MemMetaStoreConcurrencyTest, ConcurrentMoveToSameTarget) {
  // Set up: root/a/file1, root/b/file2, root/dst/
  SwordFsInode dir_a, dir_b, dir_dst;
  store_->Transact([&](MemMetaTxn &txn) {
    return txn.AddEntry(kRoot, "a", kDir, &dir_a);
  });
  store_->Transact([&](MemMetaTxn &txn) {
    return txn.AddEntry(kRoot, "b", kDir, &dir_b);
  });
  store_->Transact([&](MemMetaTxn &txn) {
    return txn.AddEntry(kRoot, "dst", kDir, &dir_dst);
  });

  SwordFsInode f1, f2;
  store_->Transact([&](MemMetaTxn &txn) {
    return txn.AddEntry(dir_a.ino, "file", kRegFile, &f1);
  });
  store_->Transact([&](MemMetaTxn &txn) {
    return txn.AddEntry(dir_b.ino, "file", kRegFile, &f2);
  });

  std::atomic<int> moved{0};
  std::atomic<int> failed{0};
  std::barrier gate(2);

  auto mover_a = [&]() {
    gate.arrive_and_wait();
    Status status = store_->Transact([&](MemMetaTxn &txn) {
      return txn.MoveEntry(dir_a.ino, "file", dir_dst.ino, "winner", false);
    });
    if (status.ok()) {
      moved.fetch_add(1, std::memory_order_relaxed);
    } else {
      failed.fetch_add(1, std::memory_order_relaxed);
    }
  };

  auto mover_b = [&]() {
    gate.arrive_and_wait();
    Status status = store_->Transact([&](MemMetaTxn &txn) {
      return txn.MoveEntry(dir_b.ino, "file", dir_dst.ino, "winner", false);
    });
    if (status.ok()) {
      moved.fetch_add(1, std::memory_order_relaxed);
    } else {
      failed.fetch_add(1, std::memory_order_relaxed);
    }
  };

  std::thread t1(mover_a);
  std::thread t2(mover_b);
  t1.join();
  t2.join();

  EXPECT_EQ(moved.load(), 1)
      << "TOCTOU: two moves to the same target both succeeded";
  EXPECT_EQ(failed.load(), 1);

  // Verify exactly one file ended up at the target
  SwordFsInode winner;
  Status status = store_->Transact([&](MemMetaTxn &txn) {
    return txn.LookupEntry(dir_dst.ino, "winner", &winner);
  });
  EXPECT_TRUE(status.ok());
  bool winner_is_f1 = (winner.ino == f1.ino);
  bool winner_is_f2 = (winner.ino == f2.ino);
  EXPECT_TRUE(winner_is_f1 || winner_is_f2);
}
