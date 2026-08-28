// Copyright 2026 SwordFS Contributors.
// Licensed under the Apache License, Version 2.0.

#pragma once

#include <cstdint>
#include <memory>
#include <string>
#include <string_view>

#include "utils/Status.hpp"

namespace swordfs::metadata {

struct SwordFsAttr;

enum class RecordType : uint32_t {
  kVolume = 1,
  kInode = 2,
  kEntry = 3,
  kChunk = 4,
};

class BufEncoder {
 public:
  BufEncoder();
  ~BufEncoder();
  BufEncoder(BufEncoder &&) noexcept;
  BufEncoder &operator=(BufEncoder &&) noexcept;
  BufEncoder(const BufEncoder &) = delete;
  BufEncoder &operator=(const BufEncoder &) = delete;

  void U32(uint32_t value);
  void U64(uint64_t value);
  void I64(int64_t value);
  void String(std::string_view value);
  void Attr(const SwordFsAttr &attr);
  void Header(RecordType type);
  void Finish(std::string *out);

 private:
  class Impl;
  std::unique_ptr<Impl> impl_;
};

class BufDecoder {
 public:
  explicit BufDecoder(std::string_view data);
  ~BufDecoder();
  BufDecoder(BufDecoder &&) noexcept;
  BufDecoder &operator=(BufDecoder &&) noexcept;
  BufDecoder(const BufDecoder &) = delete;
  BufDecoder &operator=(const BufDecoder &) = delete;

  bool U32(uint32_t *value);
  bool U64(uint64_t *value);
  bool I64(int64_t *value);
  bool String(std::string *value);
  bool Attr(SwordFsAttr *attr);
  bool Header(RecordType expected_type);
  bool Done() const;
  explicit operator bool() const noexcept;

 private:
  class Impl;
  std::unique_ptr<Impl> impl_;
};

}  // namespace swordfs::metadata
