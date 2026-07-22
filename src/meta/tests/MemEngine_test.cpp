// Copyright 2026 SwordFS Contributors.
// Licensed under the Apache License, Version 2.0.

// Unit tests for MemEngine — correctness + concurrent transaction validation.

#include <algorithm>
#include <atomic>
#include <cassert>
#include <cstdio>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

#include "meta/KeyBuilder.hpp"
#include "meta/MemEngine.hpp"

using namespace swordfs::meta;

// ── Test helpers ───────────────────────────────────────────────────────────

static int tests_run = 0;
static int tests_passed = 0;
static int tests_failed = 0;

#define TEST(name)                                         \
  static void test_##name();                                \
  struct TestReg_##name {                                   \
    TestReg_##name() {                                      \
      printf("  RUN  %s\n", #name);                         \
      tests_run++;                                          \
      test_##name();                                        \
      printf("  PASS %s\n", #name);                         \
      tests_passed++;                                       \
    }                                                        \
  };                                                         \
  static TestReg_##name reg_##name;                           \
  static void test_##name()

#define ASSERT(cond)                                         \
  do {                                                       \
    if (!(cond)) {                                           \
      printf("  FAIL at %s:%d: %s\n", __FILE__, __LINE__,   \
             #cond);                                         \
      tests_failed++;                                        \
      return;                                                \
    }                                                        \
  } while (0)

#define ASSERT_EQ(a, b)                                      \
  do {                                                       \
    if ((a) != (b)) {                                        \
      printf("  FAIL at %s:%d: %s != %s\n", __FILE__,       \
             __LINE__, #a, #b);                              \
      printf("    left:  %s\n", std::to_string(a).c_str());  \
      printf("    right: %s\n", std::to_string(b).c_str());  \
      tests_failed++;                                        \
      return;                                                \
    }                                                        \
  } while (0)

// ── Basic CRUD tests ───────────────────────────────────────────────────────

TEST(basic_get_set) {
  MemEngine engine;

  // Set and get a key.
  ASSERT(engine.Set("hello", "world") == EngineError::kOk);
  auto val = engine.Get("hello");
  ASSERT(val.has_value());
  ASSERT_EQ(*val, "world");

  // Get a non-existent key.
  auto missing = engine.Get("nonexistent");
  ASSERT(!missing.has_value());
}

TEST(basic_delete) {
  MemEngine engine;

  ASSERT(engine.Set("key", "value") == EngineError::kOk);
  ASSERT(engine.Get("key").has_value());

  ASSERT(engine.Delete("key") == EngineError::kOk);
  ASSERT(!engine.Get("key").has_value());

  // Delete non-existent key is idempotent.
  ASSERT(engine.Delete("nonexistent") == EngineError::kOk);
}

TEST(basic_scan) {
  MemEngine engine;

  engine.Set("a1", "v1");
  engine.Set("a2", "v2");
  engine.Set("a3", "v3");
  engine.Set("b1", "v4");

  std::vector<std::string> scanned;
  engine.Scan("a1", "a3", [&](std::string_view k, std::string_view v) {
    scanned.push_back(std::string(k));
    return true;
  });

  // Should include a1 and a2 (a3 is exclusive end).
  ASSERT_EQ(scanned.size(), 2u);
  ASSERT_EQ(scanned[0], "a1");
  ASSERT_EQ(scanned[1], "a2");
}

TEST(basic_exists) {
  MemEngine engine;

  engine.Set("prefix:key1", "v1");
  engine.Set("prefix:key2", "v2");
  engine.Set("other:key", "v3");

  ASSERT(engine.Exists("prefix:"));
  ASSERT(engine.Exists("other:"));
  ASSERT(!engine.Exists("nonexistent:"));
}

// ── KeyBuilder integration tests ──────────────────────────────────────────

TEST(keybuilder_inode_key) {
  auto key = KeyBuilder::InodeKey(42);
  uint64_t inode;
  ASSERT(KeyBuilder::ParseInodeKey(key, &inode));
  ASSERT_EQ(inode, 42u);
}

TEST(keybuilder_entry_key_sorting) {
  // Directory entries must sort correctly for prefix scan.
  auto k1 = KeyBuilder::EntryKey(1, "alpha");
  auto k2 = KeyBuilder::EntryKey(1, "beta");
  auto k3 = KeyBuilder::EntryKey(1, "gamma");

  ASSERT(k1 < k2);
  ASSERT(k2 < k3);

  // Entries under different parents sort by parent first.
  auto k4 = KeyBuilder::EntryKey(2, "alpha");
  ASSERT(k1 < k4);  // parent 1 < parent 2
}

TEST(keybuilder_entry_scan_prefix) {
  auto prefix = KeyBuilder::EntryScanPrefix(1);

  MemEngine engine;
  engine.Set(KeyBuilder::EntryKey(1, "file1"), "100");
  engine.Set(KeyBuilder::EntryKey(1, "file2"), "200");
  engine.Set(KeyBuilder::EntryKey(2, "file1"), "300");  // Different parent.

  std::vector<std::string> entries;
  engine.Scan(prefix, prefix + "\xFF", [&](std::string_view k,
                                            std::string_view v) {
    uint64_t parent;
    std::string_view name;
    ASSERT(KeyBuilder::ParseEntryKey(k, &parent, &name));
    ASSERT_EQ(parent, 1u);
    entries.push_back(std::string(name));
    return true;
  });

  ASSERT_EQ(entries.size(), 2u);
  ASSERT_EQ(entries[0], "file1");
  ASSERT_EQ(entries[1], "file2");
}

// ── ID allocation tests ───────────────────────────────────────────────────

TEST(id_allocation_monotonic) {
  MemEngine engine;

  uint64_t id1 = engine.NextId();
  uint64_t id2 = engine.NextId();
  uint64_t id3 = engine.NextId();

  ASSERT(id1 < id2);
  ASSERT(id2 < id3);
}

// ── Transaction tests ─────────────────────────────────────────────────────

TEST(txn_basic_commit) {
  MemEngine engine;

  auto txn = engine.BeginTxn();
  txn->Set("key1", "value1");
  txn->Set("key2", "value2");
  ASSERT(txn->Commit() == EngineError::kOk);

  ASSERT_EQ(*engine.Get("key1"), "value1");
  ASSERT_EQ(*engine.Get("key2"), "value2");
}

TEST(txn_read_your_writes) {
  MemEngine engine;

  auto txn = engine.BeginTxn();
  txn->Set("key", "new_value");
  auto val = txn->Get("key");
  ASSERT(val.has_value());
  ASSERT_EQ(*val, "new_value");
  txn->Commit();
}

TEST(txn_conflict_detection) {
  MemEngine engine;
  engine.Set("counter", "0");

  // Txn1 reads counter=0.
  auto txn1 = engine.BeginTxn();
  auto v1 = txn1->Get("counter");
  ASSERT(v1.has_value());
  ASSERT_EQ(*v1, "0");

  // Another write increments counter outside the transaction.
  engine.Set("counter", "1");

  // Txn1 tries to write and commit — should conflict.
  txn1->Set("counter", "100");
  ASSERT(txn1->Commit() == EngineError::kConflict);

  // The value should be the external write, not the txn.
  ASSERT_EQ(*engine.Get("counter"), "1");
}

TEST(txn_incr_by) {
  MemEngine engine;
  engine.Set("counter", "0");

  auto txn = engine.BeginTxn();
  txn->IncrBy("counter", 1024);
  ASSERT(txn->Commit() == EngineError::kOk);

  ASSERT_EQ(*engine.Get("counter"), "1024");
}

// ── Concurrent transaction test ───────────────────────────────────────────

TEST(concurrent_transactions) {
  MemEngine engine;
  engine.Set("shared", "0");

  constexpr int kThreads = 4;
  constexpr int kPerThread = 100;
  std::atomic<int> conflicts{0};
  std::atomic<int> successes{0};

  auto worker = [&]() {
    for (int i = 0; i < kPerThread; ++i) {
      for (int retry = 0; retry < 10; ++retry) {
        auto txn = engine.BeginTxn();
        auto val = txn->Get("shared");
        int64_t current = std::stoll(*val);
        txn->Set("shared", std::to_string(current + 1));
        EngineError result = txn->Commit();
        if (result == EngineError::kOk) {
          successes++;
          break;
        } else if (result == EngineError::kConflict) {
          conflicts++;
          continue;
        }
      }
    }
  };

  std::vector<std::thread> threads;
  for (int i = 0; i < kThreads; ++i) {
    threads.emplace_back(worker);
  }
  for (auto& t : threads) {
    t.join();
  }

  // Final value should be exactly kThreads * kPerThread (no lost updates).
  auto final_val = engine.Get("shared");
  ASSERT(final_val.has_value());
  int64_t total = std::stoll(*final_val);
  ASSERT_EQ(total, static_cast<int64_t>(kThreads * kPerThread));
}

// ── Main ───────────────────────────────────────────────────────────────────

int main() {
  printf("=== MemEngine Unit Tests ===\n\n");

  // Tests run automatically via static registration.

  printf("\n=== Results: %d run, %d passed, %d failed ===\n",
         tests_run, tests_passed, tests_failed);

  return tests_failed > 0 ? 1 : 0;
}
