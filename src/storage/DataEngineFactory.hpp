// Copyright 2026 SwordFS Contributors.
// Licensed under the Apache License, Version 2.0.

// DataEngineFactory — creates a configured IDataEngine from a VolumeConfig.

#pragma once

#include <memory>

#include "utils/Status.hpp"

namespace swordfs {

namespace volume {
struct VolumeConfig;
}

namespace storage {

class IDataEngine;

/// Create the data engine described by a VolumeConfig.
///
/// |out| is set to nullptr when vol.bucket is empty (no data engine
/// needed).  Returns an error Status when the bucket URL is invalid
/// or the scheme is unsupported.
utils::Status CreateDataEngine(const volume::VolumeConfig &vol,
                               std::unique_ptr<IDataEngine> *out);

}  // namespace storage
}  // namespace swordfs
