// Copyright 2026 SwordFS Contributors.
// Licensed under the Apache License, Version 2.0.

#include "tests/e2e/Utils.hpp"

#include <cstdint>

namespace swordfs::e2e {

std::string MakeDeterministicPayload(size_t size) {
  std::string data(size, '\0');
  uint64_t state = 0x9e3779b97f4a7c15ULL;
  for (size_t i = 0; i < size; ++i) {
    // xorshift64*: deterministic, cheap, and position-sensitive enough that
    // reordered/repeated blocks or swapped chunk objects change the hash.
    state ^= state >> 12;
    state ^= state << 25;
    state ^= state >> 27;
    state *= 0x2545f4914f6cdd1dULL;
    data[i] = static_cast<char>(state >> 56);
  }
  return data;
}

}  // namespace swordfs::e2e
