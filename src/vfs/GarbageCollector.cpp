// Copyright 2026 SwordFS Contributors.
// Licensed under the Apache License, Version 2.0.

#include "vfs/GarbageCollector.hpp"

#include <folly/logging/xlog.h>

#include <vector>

#include "metadata/IMetaEngine.hpp"
#include "storage/IDataEngine.hpp"
#include "utils/Logging.hpp"

namespace swordfs::vfs {

GarbageCollector::GarbageCollector(metadata::IMetaEngine *meta, storage::IDataEngine *data) : meta_(meta), data_(data) {
}

utils::Status GarbageCollector::CollectOne(metadata::InodeID ino) {
  utils::Status first_error = utils::Status::OK();
  auto status = meta_->VisitReclaimChunks(ino, [&](const metadata::SwordFsChunk &chunk) {
    auto delete_status = data_->Delete(chunk.key);
    if (!delete_status.ok()) {
      if (first_error.ok()) {
        first_error = delete_status;
      }
      SWORDFS_LOG_WARN << "GarbageCollector: Delete(" << chunk.key << ") for inode " << ino
                       << " failed: " << delete_status.message();
    }
    // Continue through the frozen chunk list so one bad object does not
    // prevent independent objects from being reclaimed in this pass.
    return utils::Status::OK();
  });
  if (!status.ok()) {
    return status;
  }
  if (!first_error.ok()) {
    return first_error;
  }
  return meta_->CompleteReclaim(ino);
}

utils::Status GarbageCollector::Reclaim(metadata::InodeID ino) {
  auto status = meta_->ReclaimInode(ino);
  if (!status.ok()) {
    return status;
  }
  return CollectOne(ino);
}

utils::Status GarbageCollector::Reconcile() {
  // Snapshot identifiers before completing jobs. Redis HSCAN does not promise
  // stable iteration when fields are removed from the hash being scanned.
  std::vector<metadata::InodeID> pending;
  auto status = meta_->VisitPendingReclaims([&](metadata::InodeID ino) {
    pending.push_back(ino);
    return utils::Status::OK();
  });
  if (!status.ok()) {
    return status;
  }

  utils::Status first_error = utils::Status::OK();
  for (metadata::InodeID ino : pending) {
    auto collect_status = CollectOne(ino);
    if (!collect_status.ok()) {
      if (first_error.ok()) {
        first_error = collect_status;
      }
      SWORDFS_LOG_WARN << "GarbageCollector: pending inode " << ino << " remains queued: " << collect_status.message();
    }
  }
  return first_error;
}

utils::Status GarbageCollector::Recover() {
  // As with pending jobs, snapshot orphan candidates before ReclaimInode
  // removes their hash fields.
  std::vector<metadata::InodeID> orphaned;
  auto status = meta_->VisitOrphanedInodes([&](metadata::InodeID ino) {
    orphaned.push_back(ino);
    return utils::Status::OK();
  });
  if (!status.ok()) {
    return status;
  }

  utils::Status first_error = utils::Status::OK();
  for (metadata::InodeID ino : orphaned) {
    auto reclaim_status = meta_->ReclaimInode(ino);
    if (!reclaim_status.ok()) {
      if (first_error.ok()) {
        first_error = reclaim_status;
      }
      SWORDFS_LOG_WARN << "GarbageCollector: failed to prepare orphan inode " << ino << ": "
                       << reclaim_status.message();
    }
  }

  status = Reconcile();
  if (first_error.ok()) {
    first_error = status;
  }
  return first_error;
}

}  // namespace swordfs::vfs
