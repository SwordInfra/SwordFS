// Copyright 2026 SwordFS Contributors.
// Licensed under the Apache License, Version 2.0.

#pragma once

#include <cstdint>
#include <initializer_list>
#include <limits>
#include <string>

namespace swordfs::test::posix_acl {

inline constexpr uint16_t kUserObj = 0x01;
inline constexpr uint16_t kUser = 0x02;
inline constexpr uint16_t kGroupObj = 0x04;
inline constexpr uint16_t kGroup = 0x08;
inline constexpr uint16_t kMask = 0x10;
inline constexpr uint16_t kOther = 0x20;
inline constexpr uint32_t kUndefinedId = std::numeric_limits<uint32_t>::max();

struct Entry {
  uint16_t tag;
  uint16_t perm;
  uint32_t id = kUndefinedId;
};

inline void AppendLe16(std::string *out, uint16_t value) {
  out->push_back(static_cast<char>(value & 0xff));
  out->push_back(static_cast<char>((value >> 8) & 0xff));
}

inline void AppendLe32(std::string *out, uint32_t value) {
  out->push_back(static_cast<char>(value & 0xff));
  out->push_back(static_cast<char>((value >> 8) & 0xff));
  out->push_back(static_cast<char>((value >> 16) & 0xff));
  out->push_back(static_cast<char>((value >> 24) & 0xff));
}

inline std::string Encode(std::initializer_list<Entry> entries) {
  std::string out;
  out.reserve(4 + entries.size() * 8);
  AppendLe32(&out, 2);
  for (const auto &entry : entries) {
    AppendLe16(&out, entry.tag);
    AppendLe16(&out, entry.perm);
    AppendLe32(&out, entry.id);
  }
  return out;
}

}  // namespace swordfs::test::posix_acl
