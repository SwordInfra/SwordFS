// Copyright 2026 SwordFS Contributors.
// Licensed under the Apache License, Version 2.0.

// VolumeImpl — volume lifecycle logic (format, mount).
//
// Owns a SwordFsVolume, the metadata engine, and the data engine.
// VfsImpl binds to a VolumeImpl to access all volume-level resources.

#pragma once

#include <memory>
#include <optional>
#include <string_view>

#include "metadata/types/Volume.hpp"
#include "utils/Status.hpp"

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

  // ────────────────────────────────────────────────────────────────
  // Singleton — the VolumeImpl is immutable after mount, so a global
  // access point eliminates parameter threading.
  // ────────────────────────────────────────────────────────────────

  /// Initialize the singleton.  Must be called exactly once during
  /// mount, before any other code accesses Instance().  Also serves
  /// as a reset (testing only).
  static void Initialize();

  /// Return the singleton instance.  Must be called after Initialize().
  static VolumeImpl &Instance();

  // ────────────────────────────────────────────────────────────────
  // Lifecycle
  // ────────────────────────────────────────────────────────────────

  /// Build and persist volume configuration, then format the metadata engine.
  Status CreateFrom(const swordfs::config::ConfigCenter &cfg);

  /// Load volume configuration from the metadata backend or volume.fmt for
  /// memory mode, then initialise both engines.
  Status LoadFrom(const swordfs::config::ConfigCenter &cfg);

  /// Explicitly tear down engines before static destruction.  Must be
  /// called before the process exits to avoid blocking in
  /// Aws::ShutdownAPI() when AWS SDK resources are still alive.
  void Shutdown();

  const swordfs::metadata::SwordFsVolume &config() const { return config_; }

  /// Chunk size in bytes — normally immutable after format, but see
  /// `set_chunk_size_for_test` for the unit-test escape hatch.
  uint64_t chunk_size() const {
    return chunk_size_override_.value_or(config_.chunk_size);
  }

  // Test-only override: lets unit tests shrink the chunk size so a
  // single Write across multiple chunks doesn't need to push tens of
  // MiB through the I/O stack. Production code paths never call this.
  void set_chunk_size_for_test(uint64_t cs) { chunk_size_override_ = cs; }

  // Test-only: clear the override so chunk_size() falls back to
  // config_.chunk_size again.
  void clear_chunk_size_for_test() { chunk_size_override_.reset(); }

  swordfs::metadata::IMetaEngine *meta_engine() const {
    return meta_engine_.get();
  }
  swordfs::storage::IDataEngine *data_engine() const {
    return data_engine_.get();
  }

  // ────────────────────────────────────────────────────────────────
  // Testing only — inject mock engines before Bind().
  // ────────────────────────────────────────────────────────────────
  void set_meta_engine(std::unique_ptr<swordfs::metadata::IMetaEngine> meta);
  void set_data_engine(std::unique_ptr<swordfs::storage::IDataEngine> data);

 private:
  swordfs::metadata::SwordFsVolume config_;
  // Test-only override of config_.chunk_size; std::nullopt means
  // "use config_.chunk_size". Production code never sets this.
  std::optional<uint64_t> chunk_size_override_;
  std::unique_ptr<swordfs::metadata::IMetaEngine> meta_engine_;
  std::unique_ptr<swordfs::storage::IDataEngine> data_engine_;

  static std::unique_ptr<VolumeImpl> instance_;
};

}  // namespace volume
}  // namespace swordfs
