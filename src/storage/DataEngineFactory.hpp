// Copyright 2026 SwordFS Contributors.
// Licensed under the Apache License, Version 2.0.

// DataEngineFactory — routes a VolumeConfig to the appropriate
// concrete IDataEngine based on the bucket URL's scheme.

#pragma once

#include <memory>

#include "utils/Status.hpp"

namespace swordfs {

namespace volume {
struct VolumeConfig;
}

namespace storage {

class IDataEngine;

/// Build the data engine described by |vol|, dispatching to the
/// concrete engine based on the bucket URL scheme.
utils::Status CreateDataEngine(const volume::VolumeConfig &vol,
                               std::unique_ptr<IDataEngine> *out);

}  // namespace storage
}  // namespace swordfs
