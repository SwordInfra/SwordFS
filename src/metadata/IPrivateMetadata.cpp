// Copyright 2026 SwordFS Contributors.
// Licensed under the Apache License, Version 2.0.

#include "metadata/IPrivateMetadata.hpp"

namespace swordfs::metadata::internal {

utils::Status AllocatePrivateSequenceValue(uint64_t *current, uint64_t *value) {
  if (current == nullptr || value == nullptr) {
    return utils::Status::InvalidArgument("private sequence state or output is null");
  }
  if (*current >= kMaxPrivateSequenceValue) {
    return utils::Status::IOError("private sequence exhausted");
  }
  ++*current;
  *value = *current;
  return utils::Status::OK();
}

}  // namespace swordfs::metadata::internal
