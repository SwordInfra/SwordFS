// Copyright 2026 SwordFS Contributors.
// Licensed under the Apache License, Version 2.0.

// OpenHandleTracker — runtime handle accounting exposed to the metadata
// engine without leaking VFS-layer dependencies.
//
// The metadata engine owns POSIX directory / inode semantics. POSIX
// `unlink(2)` semantics require the engine to defer inode deletion when an
// open file descriptor still references the inode (open-unlink). Whether
// any descriptor is still open is **runtime client state**, not filesystem
// state — historically SwordFS queried the VFS layer (`InodeHandleManager`)
// directly, which created a metadata→vfs layering cycle and a fragile
// cross-layer lock order.
//
// `OpenHandleTracker` decouples the two: the metadata engine asks the
// tracker "do you still have open handles?" and the tracker answers
// without the engine ever knowing what an `InodeHandle` is. Concrete
// trackers live in the VFS layer (`vfs/InodeHandleManager.cpp`); the
// metadata layer only consumes the interface.
//
// Implementations:
//   - Production: `vfs::InodeHandleManager` (mounted FUSE daemon).
//   - Tests: a fake tracker that returns whatever the test sets.

#pragma once

#include "metadata/Types.hpp"

namespace swordfs::metadata {

class OpenHandleTracker {
 public:
  virtual ~OpenHandleTracker() = default;

  // Return true if any open file descriptor currently references |ino|.
  // The call MUST NOT take any metadata-engine mutex; it may take a
  // short-lived runtime lock (or be lock-free via atomics) but must
  // return promptly.
  virtual bool HasOpenHandles(InodeID ino) = 0;

  // Mark the inode as orphaned if open handles still reference it. The
  // implementation is responsible for the atomic check-and-set (no
  // racing with `Close`); it must be safe to call from the metadata
  // engine while the metadata engine mutex is held.
  virtual void MarkOrphaned(InodeID ino) = 0;
};

}  // namespace swordfs::metadata
