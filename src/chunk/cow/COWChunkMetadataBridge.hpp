// Copyright 2026 SwordFS Contributors.
// Licensed under the Apache License, Version 2.0.

#pragma once

#include <memory>

#include "chunk/internal/ChunkMetadataBridge.hpp"
#include "metadata/IPrivateMetadata.hpp"

namespace swordfs::chunk::cow {

utils::Status CreateCOWChunkMetadataBridge(metadata::MechanismPrivateStorePtr private_metadata,
                                           std::unique_ptr<internal::ChunkMetadataBridge> *out);

}  // namespace swordfs::chunk::cow
