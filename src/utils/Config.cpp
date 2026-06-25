// Copyright 2026 SwordFS Contributors.
// Licensed under the Apache License, Version 2.0.

#include "utils/Config.hpp"

#include <cstring>

namespace swordfs::utils {

ConfigCenter& ConfigCenter::Instance() {
  static ConfigCenter instance;
  return instance;
}

void ConfigCenter::ParseFromArgs(int argc, char* argv[]) {
  for (int i = 1; i < argc; ++i) {
    if (std::strcmp(argv[i], "-f") == 0 ||
        std::strcmp(argv[i], "--foreground") == 0) {
      foreground_ = true;
    } else if (std::strcmp(argv[i], "--log-file") == 0 && i + 1 < argc) {
      log_.path = argv[++i];
    } else if (std::strcmp(argv[i], "--log-level") == 0 && i + 1 < argc) {
      log_.level = argv[++i];
    }
  }
}

}  // namespace swordfs::utils
