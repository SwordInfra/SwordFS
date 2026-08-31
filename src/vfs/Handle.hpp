// Copyright 2026 SwordFS Contributors.
// Licensed under the Apache License, Version 2.0.

#pragma once

#include <cstdint>
#include <memory>
#include <shared_mutex>

#include "utils/Status.hpp"

namespace swordfs::vfs {

// Common state for all per-open file-system handles. The handle id is
// assigned by HandleManager when the handle is registered.
class Handle {
 public:
  virtual ~Handle() = default;

  uint64_t fh() const {
    return fh_;
  }

 private:
  friend class HandleManager;

  uint64_t fh_ = 0;
};

// Opaque map type, defined in Handle.cpp to avoid pulling Folly's F14FastMap
// into every translation unit that includes this header.
struct HandleMap;

class HandleManager {
 public:
  static HandleManager &Instance();

  /// Assign a fresh handle id and register |handle|. Returns the assigned fh.
  uint64_t Register(std::shared_ptr<Handle> handle);

  /// Unregister |fh| from the registry.
  void Unregister(uint64_t fh);

  /// Find the handle for |fh| and cast it to |T|. Returns nullptr if the
  /// handle does not exist or has a different type.
  template <typename T>
  std::shared_ptr<T> FindAs(uint64_t fh) {
    return std::dynamic_pointer_cast<T>(FindHandle(fh));
  }

 private:
  HandleManager();

  std::shared_ptr<Handle> FindHandle(uint64_t fh);

 private:
  mutable std::shared_mutex mutex_;
  uint64_t next_fh_{1};
  std::unique_ptr<HandleMap> handles_;
};

}  // namespace swordfs::vfs
