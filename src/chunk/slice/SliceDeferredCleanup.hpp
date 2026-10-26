// Copyright 2026 SwordFS Contributors.
// Licensed under the Apache License, Version 2.0.

#pragma once

#include <memory>

#include "utils/Status.hpp"

namespace swordfs::chunk::internal {
class ChunkCleanupWorkEncoder;
}

namespace swordfs::chunk::slice {

// Until #273 supplies reachability-safe physical reclaim, detached Slice
// identities are recorded durably but never consumed by a GC participant.
utils::Status CreateDeferredSliceCleanupEncoder(std::unique_ptr<internal::ChunkCleanupWorkEncoder> *out);

}  // namespace swordfs::chunk::slice
