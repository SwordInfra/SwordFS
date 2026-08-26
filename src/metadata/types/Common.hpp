// Copyright 2026 SwordFS Contributors.
// Licensed under the Apache License, Version 2.0.

#pragma once

#include <sys/types.h>

#include <cstdint>

namespace swordfs::metadata {

using InodeID = uint64_t;
using ChunkIndex = uint32_t;

constexpr InodeID kRootInodeId = 1;

struct Limits {
  size_t max_name_length;
  size_t max_free_inodes;
};

enum class SetAttrField : uint32_t {
  kMode = 1u << 0,
  kUid = 1u << 1,
  kGid = 1u << 2,
  kSize = 1u << 3,
  kAtime = 1u << 4,
  kMtime = 1u << 5,
  kAtimeNow = 1u << 7,
  kMtimeNow = 1u << 8,
  kCtime = 1u << 10,
};

enum class RenameFlag : uint32_t {
  kNone = 0,
  kNoReplace = 1u << 0,
  kExchange = 1u << 1,
};

struct RenameResult {
  InodeID overwritten_ino = 0;
  nlink_t overwritten_post_nlink = 0;
};

inline bool HasSetAttrField(SetAttrField fields, SetAttrField field) {
  return (static_cast<uint32_t>(fields) & static_cast<uint32_t>(field)) != 0;
}

inline SetAttrField operator|(SetAttrField a, SetAttrField b) {
  return static_cast<SetAttrField>(static_cast<uint32_t>(a) | static_cast<uint32_t>(b));
}

inline SetAttrField &operator|=(SetAttrField &a, SetAttrField b) {
  a = a | b;
  return a;
}

inline bool HasRenameFlag(RenameFlag flags, RenameFlag flag) {
  return (static_cast<uint32_t>(flags) & static_cast<uint32_t>(flag)) != 0;
}

inline RenameFlag operator|(RenameFlag a, RenameFlag b) {
  return static_cast<RenameFlag>(static_cast<uint32_t>(a) | static_cast<uint32_t>(b));
}

inline RenameFlag &operator|=(RenameFlag &a, RenameFlag b) {
  a = a | b;
  return a;
}

}  // namespace swordfs::metadata
