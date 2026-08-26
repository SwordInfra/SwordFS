// Copyright 2026 SwordFS Contributors.
// Licensed under the Apache License, Version 2.0.

#pragma once

#include <optional>
#include <string>
#include <string_view>

#include "utils/Status.hpp"

namespace swordfs::metadata {

/// Engine-independent metadata transaction over opaque key/value data.
/// Metadata types own serialization; engines only provide transactional
/// persistence and do not interpret values.
class MetadataTxn {
 public:
  virtual ~MetadataTxn() = default;

  virtual utils::Status Get(std::string_view key,
                            std::optional<std::string>* value) = 0;
  virtual utils::Status Put(std::string_view key, std::string_view value) = 0;
  virtual utils::Status Delete(std::string_view key) = 0;
};

}  // namespace swordfs::metadata
