// Copyright 2026 SwordFS Contributors.
// Licensed under the Apache License, Version 2.0.

// DataEngineFactory — creates a configured IDataEngine from a VolumeConfig.

#pragma once

#include <memory>

namespace swordfs {

namespace volume {
struct VolumeConfig;
}

namespace storage {

class IDataEngine;

/// Create the data engine described by a VolumeConfig.
///
/// Returns nullptr when:
/// - vol.bucket is empty (memory-only, no data engine needed), or
/// - the scheme is unknown / unsupported.
///
/// The caller (Mount.cpp) does not need to know which backends are
/// available — the registry handles that.
std::unique_ptr<IDataEngine> CreateDataEngine(const volume::VolumeConfig& vol);

}  // namespace storage
}  // namespace swordfs
