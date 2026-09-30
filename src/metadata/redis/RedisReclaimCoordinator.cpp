// Copyright 2026 SwordFS Contributors.
// Licensed under the Apache License, Version 2.0.

#include "metadata/redis/RedisReclaimCoordinator.hpp"

namespace swordfs::metadata::redis::internal {

utils::Status RunReclaimWithOutcomeReconciliation(int max_attempts, const ReclaimAttemptFn &attempt_reclaim,
                                                  const ReclaimInodeLookupFn &lookup_inode) {
  if (max_attempts <= 0 || !attempt_reclaim || !lookup_inode) {
    return utils::Status::InvalidArgument("invalid Redis reclaim reconciliation inputs");
  }

  utils::Status ambiguous;
  for (int attempt = 0; attempt < max_attempts; ++attempt) {
    auto status = attempt_reclaim();
    if (!status.IsOutcomeUnknown()) {
      return status;
    }
    ambiguous = status;

    SwordFsInode inode;
    status = lookup_inode(&inode);
    if (status.IsNotFound()) {
      // The inode key is the non-revivability fence. Missing means logical
      // reclaim crossed its point of no return even if cleanup handoff leaked.
      return utils::Status::OK();
    }
    if (!status.ok()) {
      return status;
    }
    if (inode.attr.nlink != 0) {
      // Revival won. No destructive work from the ambiguous attempt may be
      // inferred from an optional cleanup record.
      return utils::Status::OK();
    }
    // Still unlinked and live: retry from a fresh WATCHed inode/mapping
    // snapshot. A previous ambiguous attempt is not replay authority.
  }
  return ambiguous;
}

}  // namespace swordfs::metadata::redis::internal
