// Copyright 2026 SwordFS Contributors.
// Licensed under the Apache License, Version 2.0.

#pragma once

#include <functional>

#include "metadata/types/Inode.hpp"
#include "utils/Status.hpp"

namespace swordfs::metadata::redis::internal {

using ReclaimAttemptFn = std::function<utils::Status()>;
using ReclaimInodeLookupFn = std::function<utils::Status(SwordFsInode *)>;

// Converge an ambiguous Redis reclaim from authoritative inode state. Cleanup
// queue state is intentionally absent from this interface: it is maintenance,
// not evidence that the FileMetadata point of no return was crossed.
utils::Status RunReclaimWithOutcomeReconciliation(int max_attempts, const ReclaimAttemptFn &attempt_reclaim,
                                                  const ReclaimInodeLookupFn &lookup_inode);

}  // namespace swordfs::metadata::redis::internal
