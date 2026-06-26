// Copyright 2026 SwordFS Contributors.
// Licensed under the Apache License, Version 2.0.

#include "utils/ConfigCenter.hpp"

#include <cstdlib>
#include <cstring>
#include <thread>

namespace swordfs::utils {

ConfigCenter& ConfigCenter::Instance() {
  static ConfigCenter instance;
  return instance;
}

Status ConfigCenter::ParseFromArgs(int argc, char* argv[]) {
  for (int i = 1; i < argc; ++i) {
    if (std::strcmp(argv[i], "-f") == 0 ||
        std::strcmp(argv[i], "--foreground") == 0) {
      foreground_ = true;
    } else if (std::strcmp(argv[i], "--log-file") == 0 && i + 1 < argc) {
      log_.path = argv[++i];
    } else if (std::strcmp(argv[i], "--log-level") == 0 && i + 1 < argc) {
      log_.level = argv[++i];
    } else if (std::strcmp(argv[i], "--backend") == 0 && i + 1 < argc) {
      vfs_backend_ = VfsBackendFromString(argv[++i]);
      if (vfs_backend_ == VfsBackend::kInvalid) {
        return Status::InvalidArgument("backend must be memory");
      }
    } else if (std::strcmp(argv[i], "--fuse-threads") == 0 && i + 1 < argc) {
      fuse_threads_ = std::atoi(argv[++i]);
      if (fuse_threads_ <= 0) {
        return Status::InvalidArgument("fuse-threads must be greater than 0");
      }
      if (fuse_threads_ > static_cast<int>(std::thread::hardware_concurrency())) {
        return Status::InvalidArgument(
            "fuse-threads exceeds CPU core count");
      }
    }
  }
  return Status::OK();
}

}  // namespace swordfs::utils
