// Copyright 2026 SwordFS Contributors.
// Licensed under the Apache License, Version 2.0.

#pragma once

#include "metadata/types/Common.hpp"
#include "utils/Status.hpp"

namespace swordfs {

namespace metadata {
class IMetaEngine;
}

namespace storage {
class IDataEngine;
}

namespace vfs {

// Coordinates metadata GC records with idempotent data-object deletion.
// Metadata remains the authority for pending work; no process-local queue is
// required for crash recovery.
class GarbageCollector {
 public:
  GarbageCollector(metadata::IMetaEngine *meta, storage::IDataEngine *data);

  // Prepare one orphaned inode and attempt its data cleanup immediately.
  utils::Status Reclaim(metadata::InodeID ino);

  // Promote orphan candidates left by a crashed mount, then process every
  // durable deletion job. Continues past individual failures and returns the
  // first error so callers can report degraded cleanup without losing work.
  utils::Status Recover();

  // Process already-prepared deletion jobs. Safe to call periodically and
  // concurrently from more than one mount because deletion is idempotent.
  utils::Status Reconcile();

 private:
  utils::Status CollectOne(metadata::InodeID ino);

  metadata::IMetaEngine *meta_;
  storage::IDataEngine *data_;
};

}  // namespace vfs
}  // namespace swordfs
