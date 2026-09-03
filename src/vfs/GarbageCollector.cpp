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
  utils::Status first_error = utils::Status::OK();

  // Fence expired sessions first. Their persistent open references must be
  // released before orphan eligibility is evaluated.
  auto status = meta_->ReapStaleSessions();
  if (!status.ok()) {
    first_error = status;
    SWORDFS_LOG_WARN << "GarbageCollector: stale-session reconciliation incomplete: " << status.message();
  }

  // Snapshot orphan identifiers before ReclaimInode removes their fields.
  // VisitOrphanedInodes exposes only inodes whose filesystem-wide open count
  // is zero, so this is safe to run from every live mount.
  std::vector<metadata::InodeID> orphaned;
  status = meta_->VisitOrphanedInodes([&](metadata::InodeID ino) {
    orphaned.push_back(ino);
    return utils::Status::OK();
  });
  if (!status.ok()) {
    if (first_error.ok()) {
      first_error = status;
    }
  } else {
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
  }

  // Snapshot prepared jobs before completing them. Redis HSCAN does not
  // promise stable iteration when fields are removed from the hash being
  // scanned.
  std::vector<metadata::InodeID> pending;
  status = meta_->VisitPendingReclaims([&](metadata::InodeID ino) {
    pending.push_back(ino);
    return utils::Status::OK();
  });
  if (!status.ok()) {
    if (first_error.ok()) {
      first_error = status;
    }
    return first_error;
  }

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
  return Reconcile();
}

}  // namespace swordfs::vfs
