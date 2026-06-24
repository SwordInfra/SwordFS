// Copyright 2026 SwordFS Contributors.
// Licensed under the Apache License, Version 2.0.

#pragma once

namespace swordfs::fuse {

constexpr unsigned kMaxWriteSize = 1048576;      // 1 MiB
constexpr unsigned kMaxReadAheadSize = 1048576;  // 1 MiB
constexpr unsigned kTimeGran = 1000;             // microsecond granularity

}  // namespace swordfs::fuse
