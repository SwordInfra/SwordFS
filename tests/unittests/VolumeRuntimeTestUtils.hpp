// Copyright 2026 SwordFS Contributors.
// Licensed under the Apache License, Version 2.0.

#pragma once

#include <cerrno>
#include <memory>
#include <string>
#include <utility>

#include "metadata/IMetaEngine.hpp"
#include "metadata/MetaEngineRegistry.hpp"
#include "metadata/mem/MemCOWChunkMetadata.hpp"
#include "metadata/types/Volume.hpp"
#include "storage/DataEngineRegistry.hpp"
#include "storage/IDataEngine.hpp"
#include "utils/Status.hpp"
#include "volume/VolumeImpl.hpp"

namespace swordfs::test {

inline constexpr std::string_view kTestMetaEngine = "swordfs-test-meta";
inline constexpr std::string_view kTestDataEngine = "swordfs-test-data";

inline std::unique_ptr<metadata::IMetaEngine> pending_meta_engine;
inline std::unique_ptr<storage::IDataEngine> pending_data_engine;
inline metadata::SwordFsVolume pending_volume;
inline storage::DataEngineOptions pending_data_options;

class StubChunkMetadata final : public metadata::ChunkMetadata {
 public:
  explicit StubChunkMetadata(metadata::ChunkType chunk_type) : chunk_type_(chunk_type) {
  }

  metadata::ChunkType Type() const override {
    return chunk_type_;
  }

  utils::Status AllocateChunkID(metadata::ChunkID *) override {
    return utils::Status::NotSupported("stub chunk metadata does not allocate IDs");
  }

 private:
  metadata::ChunkType chunk_type_;
};

inline utils::Status CreatePendingMetaEngine(std::string_view, std::string_view,
                                             std::unique_ptr<metadata::IMetaEngine> *out) {
  if (out == nullptr) {
    return utils::Status::InvalidArgument("metadata engine output is null");
  }
  if (pending_meta_engine == nullptr) {
    return utils::Status::Internal("test metadata engine was not prepared");
  }
  *out = std::move(pending_meta_engine);
  return utils::Status::OK();
}

inline utils::Status CreatePendingDataEngine(const storage::DataEngineOptions &options,
                                             std::unique_ptr<storage::IDataEngine> *out) {
  if (out == nullptr) {
    return utils::Status::InvalidArgument("data engine output is null");
  }
  if (pending_data_engine == nullptr) {
    return utils::Status::Internal("test data engine was not prepared");
  }
  pending_data_options = options;
  *out = std::move(pending_data_engine);
  return utils::Status::OK();
}

inline void RegisterTestVolumeEngines() {
  static const bool registered = [] {
    metadata::MetaEngineRegistry::Instance().Register(kTestMetaEngine, CreatePendingMetaEngine);
    storage::DataEngineRegistry::Instance().Register(kTestDataEngine, CreatePendingDataEngine);
    return true;
  }();
  (void)registered;
}

inline utils::Status LoadConfiguredTestVolume(metadata::SwordFsVolume *out) {
  if (out == nullptr) {
    return utils::Status::InvalidArgument("volume output is null");
  }
  *out = pending_volume;
  return utils::Status::OK();
}

template <typename Base>
class ConfiguredMetaEngine final : public Base {
 public:
  template <typename... Args>
  explicit ConfiguredMetaEngine(Args &&...args) : Base(std::forward<Args>(args)...) {
  }

  utils::Status LoadVolume(metadata::SwordFsVolume *out) override {
    return LoadConfiguredTestVolume(out);
  }

  utils::Status OpenChunkMetadata(metadata::ChunkType chunk_type, metadata::ChunkMetadataPtr *out) override {
    auto status = Base::OpenChunkMetadata(chunk_type, out);
    if (status.ToErrno() != ENOSYS) {
      return status;
    }
    if (out == nullptr) {
      return utils::Status::InvalidArgument("chunk metadata output is null");
    }
    if (chunk_type != metadata::ChunkType::kCow) {
      return utils::Status::NotSupported("test chunk metadata type is not implemented");
    }
    *out = std::make_shared<metadata::MemCOWChunkMetadata>();
    return utils::Status::OK();
  }

  utils::Status BindChunkMetadataBridge(chunk::internal::ChunkMetadataBridge *bridge) override {
    auto status = Base::BindChunkMetadataBridge(bridge);
    return status.ToErrno() == ENOSYS ? utils::Status::OK() : status;
  }
};

inline volume::MountOptions MakeTestMountOptions(std::string volume_name) {
  RegisterTestVolumeEngines();
  return volume::MountOptions{
      .name = std::move(volume_name),
      .meta_url = std::string(kTestMetaEngine) + "://local",
  };
}

inline utils::Status LoadTestVolumeRuntime(std::unique_ptr<metadata::IMetaEngine> meta_engine,
                                           std::unique_ptr<storage::IDataEngine> data_engine,
                                           metadata::SwordFsVolume config = {}) {
  RegisterTestVolumeEngines();
  if (config.name.empty()) {
    config.name = "unit-test-volume";
  }
  if (data_engine != nullptr) {
    config.storage = kTestDataEngine;
  } else {
    config.storage.clear();
  }

  pending_volume = std::move(config);
  pending_meta_engine = std::move(meta_engine);
  pending_data_engine = std::move(data_engine);

  auto options = MakeTestMountOptions(pending_volume.name);
  volume::VolumeImpl::Initialize();
  auto status = volume::VolumeImpl::Instance().LoadFrom(options);
  if (!status.ok()) {
    pending_meta_engine.reset();
    pending_data_engine.reset();
  }
  return status;
}

}  // namespace swordfs::test
