// Copyright 2026 SwordFS Contributors.
// Licensed under the Apache License, Version 2.0.

// VolumeImpl — volume lifecycle logic (format, mount).
//
// Owns a SwordFsVolume, the metadata engine, and the data engine.
// Runtime components access the mounted volume through the singleton.

#pragma once

#include <cstddef>
#include <memory>
#include <string>
#include <string_view>

#include "metadata/ChunkMetadata.hpp"
#include "metadata/types/Volume.hpp"
#include "utils/Status.hpp"

namespace swordfs {

namespace chunk {
class ChunkFactory;
namespace internal {
class ChunkCleanupWorkEncoder;
}  // namespace internal
}  // namespace chunk

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

struct FormatOptions {
  std::string name;
  std::string meta_url;
  std::string bucket;
  std::string region;
  uint64_t chunk_size = 64ULL * 1024 * 1024;
  metadata::ChunkType chunk_type = metadata::ChunkType::kChunkSlice;
  bool enable_posix_acl = false;
};

struct MountOptions {
  std::string name;
  std::string meta_url;
  size_t storage_thread_count = 1;
};

class VolumeImpl {
 public:
  using Status = utils::Status;

  VolumeImpl();
  ~VolumeImpl();

  // ────────────────────────────────────────────────────────────────
  // Singleton — the VolumeImpl is immutable after mount, so a global
  // access point eliminates parameter threading.
  // ────────────────────────────────────────────────────────────────

  /// Initialize an empty singleton runtime. LoadFrom() binds persisted volume
  /// state and constructs engines through the production registries.
  static void Initialize();

  /// Return the singleton instance.  Must be called after Initialize().
  static VolumeImpl &Instance();

  // ────────────────────────────────────────────────────────────────
  // Lifecycle
  // ────────────────────────────────────────────────────────────────

  /// Build and persist volume configuration, then format the metadata engine.
  Status CreateFrom(const config::ConfigCenter &config);
  Status CreateFrom(const FormatOptions &options);

  /// Load authoritative volume configuration from the metadata backend,
  /// then initialise both engines.
  Status LoadFrom(const config::ConfigCenter &config);
  Status LoadFrom(const MountOptions &options);

  /// Explicitly tear down engines before static destruction.  Must be
  /// called before the process exits to avoid blocking in
  /// Aws::ShutdownAPI() when AWS SDK resources are still alive.
  void Shutdown();

  const swordfs::metadata::SwordFsVolume &config() const {
    return config_;
  }

  /// Chunk size in bytes from the active volume configuration.
  uint64_t chunk_size() const {
    return config_.chunk_size;
  }

  swordfs::metadata::IMetaEngine *meta_engine() const {
    return meta_engine_.get();
  }
  const swordfs::chunk::ChunkFactory *chunk_factory() const {
    return chunk_factory_.get();
  }

 private:
  Status ComposeChunkMetadata();
  Status ComposeChunkRuntime();

 private:
  swordfs::metadata::SwordFsVolume config_;
  // The metadata engine borrows the mount-owned cleanup encoder. Declare
  // private ChunkMetadata and the encoder before the engine so both outlive
  // the borrowing engine during reverse-order destruction.
  swordfs::metadata::ChunkMetadataPtr chunk_metadata_;
  std::unique_ptr<swordfs::chunk::internal::ChunkCleanupWorkEncoder> cleanup_work_encoder_;
  std::unique_ptr<swordfs::metadata::IMetaEngine> meta_engine_;
  std::unique_ptr<swordfs::storage::IDataEngine> data_engine_;
  // The runtime factory borrows these engines and metadata for mount lifetime.
  std::unique_ptr<swordfs::chunk::ChunkFactory> chunk_factory_;

  static std::unique_ptr<VolumeImpl> instance_;
};

}  // namespace volume
}  // namespace swordfs
