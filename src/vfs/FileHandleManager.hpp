// Copyright 2026 SwordFS Contributors.
// Licensed under the Apache License, Version 2.0.

// FileHandleManager — singleton mapping fh → FileReadWriter.
// Thread-safe: all public methods acquire the appropriate lock.

#pragma once

#include <folly/container/F14Map.h>

#include <cstdint>
#include <memory>
#include <mutex>
#include <shared_mutex>

#include "metadata/Types.hpp"
#include "utils/Status.hpp"
#include "vfs/FileReadWriter.hpp"

namespace swordfs {
namespace volume {
class VolumeImpl;
}
}  // namespace swordfs

namespace swordfs::vfs {

class FileHandleManager {
 public:
  static FileHandleManager& Instance();

  /// Create a FileReadWriter from |vol| and associate it with |fh|.
  /// Returns kAlreadyExists if |fh| is already open.
  utils::Status Open(uint64_t fh, volume::VolumeImpl* vol,
                     metadata::InodeID ino);

  /// Find the FileReadWriter for |fh|, or nullptr.
  /// The returned shared_ptr keeps the handle alive.
  std::shared_ptr<FileReadWriter> Find(uint64_t fh);

  /// Flush and remove |fh|.  Called on release / close.
  void Release(uint64_t fh);

 private:
  FileHandleManager() = default;

  mutable std::shared_mutex mutex_;
  folly::F14FastMap<uint64_t, std::shared_ptr<FileReadWriter>> files_;
};

}  // namespace swordfs::vfs
