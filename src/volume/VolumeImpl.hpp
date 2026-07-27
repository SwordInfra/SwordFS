// Copyright 2026 SwordFS Contributors.
// Licensed under the Apache License, Version 2.0.

// VolumeImpl — volume lifecycle logic (format, mount).
//
// Owns a VolumeConfig, the metadata engine, and the data engine.
// VfsImpl binds to a VolumeImpl to access all volume-level resources.

#pragma once

#include <memory>

#include "utils/Status.hpp"
#include "volume/VolumeConfig.hpp"

namespace swordfs {

namespace config {
class ConfigCenter;
}

namespace metadata {
class IMetaEngine;
}

namespace storage {
class IDataEngine;
}

namespace volume {

class VolumeImpl {
 public:
  using Status = utils::Status;

  VolumeImpl();
  ~VolumeImpl();

  /// Build config from CLI flags and persist to disk (format).
  Status CreateFrom(const swordfs::config::ConfigCenter& cfg);

  /// Load config from persistent store and initialise both engines (mount).
  Status LoadFrom(const swordfs::config::ConfigCenter& cfg);

  const VolumeConfig& config() const { return config_; }

  swordfs::metadata::IMetaEngine* meta_engine() const {
    return meta_engine_.get();
  }
  swordfs::storage::IDataEngine* data_engine() const {
    return data_engine_.get();
  }

  // ────────────────────────────────────────────────────────────────
  // Testing only — inject mock engines before Bind().
  // ────────────────────────────────────────────────────────────────
  void set_meta_engine(std::unique_ptr<swordfs::metadata::IMetaEngine> meta);
  void set_data_engine(std::unique_ptr<swordfs::storage::IDataEngine> data);

 private:
  VolumeConfig config_;
  std::unique_ptr<swordfs::metadata::IMetaEngine> meta_engine_;
  std::unique_ptr<swordfs::storage::IDataEngine> data_engine_;
};

}  // namespace volume
}  // namespace swordfs
