// Copyright 2026 SwordFS Contributors.
// Licensed under the Apache License, Version 2.0.

// MetaEngineFactory — creates a configured IMetaEngine from a meta_url string.

#pragma once

#include <memory>
#include <string_view>

#include "utils/Status.hpp"

namespace swordfs::metadata {

class IMetaEngine;

/// Create a metadata engine from a URL string.
/// On success, |out| is populated and Status::OK() is returned.
utils::Status CreateMetaEngine(std::string_view scheme, std::string_view meta_url, std::unique_ptr<IMetaEngine> *out);

}  // namespace swordfs::metadata
