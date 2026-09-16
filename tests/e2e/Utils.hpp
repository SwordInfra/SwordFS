// Copyright 2026 SwordFS Contributors.
// Licensed under the Apache License, Version 2.0.

#pragma once

#include <cstddef>
#include <string>

namespace swordfs::e2e {

/// Generate a reproducible, position-sensitive binary payload.
///
/// The byte pattern is intentionally non-periodic at ordinary block/chunk
/// boundaries so reordered, repeated, or swapped data regions remain
/// observable in E2E content verification.
std::string MakeDeterministicPayload(size_t size);

}  // namespace swordfs::e2e
