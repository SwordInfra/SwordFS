// Copyright 2026 SwordFS Contributors.
// Licensed under the Apache License, Version 2.0.

#pragma once

#include <cstdint>
#include <string>
#include <string_view>

#include "metadata/types/Common.hpp"
#include "utils/Status.hpp"

namespace swordfs::metadata {

/// One directory entry returned by IMetaEngine::ReadDir.
struct SwordFsEntry {
  std::string name;
  uint32_t type;  // DT_DIR, DT_REG, DT_LNK, etc.
  InodeID ino;

  utils::Status SerializeTo(std::string *out) const;
  utils::Status ParseFrom(std::string_view data);

};

}  // namespace swordfs::metadata
