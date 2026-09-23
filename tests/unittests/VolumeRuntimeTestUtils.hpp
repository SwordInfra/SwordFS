// Copyright 2026 SwordFS Contributors.
// Licensed under the Apache License, Version 2.0.

#pragma once

#include <cerrno>
#include <memory>
#include <string>
#include <utility>

#include "metadata/IMetaEngine.hpp"
#include "metadata/MetaEngineRegistry.hpp"
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

  utils::Status BindChunkOverwriteStrategy(const chunk::IChunkOverwriteStrategy *strategy) override {
    auto status = Base::BindChunkOverwriteStrategy(strategy);
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
