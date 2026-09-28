// Copyright 2026 SwordFS Contributors.
// Licensed under the Apache License, Version 2.0.

#pragma once

#include <folly/synchronization/LifoSem.h>

#include <atomic>
#include <thread>

#include "metadata/Types.hpp"
#include "utils/Status.hpp"

namespace swordfs::vfs {

// VFS-owned logical orphan handoff. This worker knows only inode reachability
// and local open-handle fencing; physical cleanup belongs to chunk internals.
class OrphanReclaimer {
 public:
  static OrphanReclaimer &Instance();

  OrphanReclaimer(const OrphanReclaimer &) = delete;
  OrphanReclaimer &operator=(const OrphanReclaimer &) = delete;

  utils::Status Reconcile();
  void Start();
  void Stop();
  void Wake();

 private:
  OrphanReclaimer() = default;
  ~OrphanReclaimer() = default;

  utils::Status PrepareOrphan(metadata::InodeID ino);
  void WorkerLoop();
  void RunWorkerPass();

 private:
  std::atomic<bool> stop_requested_{false};
  std::atomic<bool> wake_pending_{false};
  folly::LifoSem wake_sem_;
  std::thread worker_thread_;
};

}  // namespace swordfs::vfs
