// Copyright 2026 SwordFS Contributors.
// Licensed under the Apache License, Version 2.0.

#include "volume/VolumeImpl.hpp"

#include <folly/logging/xlog.h>

#include <algorithm>
#include <optional>

#include "chunk/ChunkFactory.hpp"
#include "chunk/internal/ChunkCleanupWorkEncoder.hpp"
#include "config/ConfigCenter.hpp"
#include "metadata/IMetaEngine.hpp"
#include "metadata/MetaEngineRegistry.hpp"
#include "storage/DataEngineRegistry.hpp"
#include "storage/IDataEngine.hpp"
#include "storage/StorageUrl.hpp"
#include "utils/ExecutionDomain.hpp"

namespace swordfs::volume {
namespace {

Status CreateMetaEngine(std::string_view meta_url, std::string_view volume_name,
                        std::unique_ptr<swordfs::metadata::IMetaEngine> *out) {
  if (out == nullptr) {
    return Status::InvalidArgument("metadata engine output is null");
  }

  utils::StorageUrl url;
  if (!utils::StorageUrl::Parse(meta_url, &url)) {
    return Status::InvalidArgument("invalid metadata URL: " + std::string(meta_url));
  }

  try {
    return swordfs::metadata::MetaEngineRegistry::Instance().CreateInstance(url.scheme, meta_url, volume_name, out);
  } catch (const std::exception &error) {
    return Status::IOError("metadata engine initialization failed: " + std::string(error.what()));
  }
}

Status CreateDataEngine(std::string_view storage, const swordfs::storage::DataEngineOptions &options,
                        std::unique_ptr<swordfs::storage::IDataEngine> *out) {
  if (storage.empty()) {
    return Status::Malformed("volume storage backend is empty");
  }
  return swordfs::storage::DataEngineRegistry::Instance().CreateInstance(storage, options, out);
}

}  // namespace

VolumeImpl::VolumeImpl() = default;
VolumeImpl::~VolumeImpl() {
  utils::ExpectInThreadDomain();
}

std::unique_ptr<VolumeImpl> VolumeImpl::instance_;

void VolumeImpl::Initialize() {
  utils::ExpectInThreadDomain();
  instance_ = std::make_unique<VolumeImpl>();
}

VolumeImpl &VolumeImpl::Instance() {
  return *instance_;
}

Status VolumeImpl::ComposeChunkMetadata() {
  chunk_metadata_.reset();
  cleanup_work_encoder_.reset();
  auto status = meta_engine_->OpenChunkMetadata(config_.chunk_type, &chunk_metadata_);
  if (!status.ok()) {
    return status;
  }
  // FileMetadata receives only opaque detached-work serialization. The
  // selected mechanism's private Slice index is not accessed by this encoder.
  if (chunk_metadata_ == nullptr || chunk_metadata_->Type() != config_.chunk_type) {
    return Status::InvalidArgument("chunk metadata type mismatch");
  }
  status = chunk::internal::CreateChunkCleanupWorkEncoder(config_.chunk_type, &cleanup_work_encoder_);
  if (!status.ok()) {
    return status;
  }
  return meta_engine_->BindChunkCleanupWorkEncoder(cleanup_work_encoder_.get());
}

Status VolumeImpl::ComposeChunkRuntime() {
  chunk_factory_.reset();
  // LoadFrom reaches runtime composition only after ComposeChunkMetadata
  // succeeds; incomplete capabilities cannot reach this point.
  chunk_factory_ = std::make_unique<chunk::ChunkFactory>(config_.chunk_type, chunk_metadata_, meta_engine_.get(),
                                                         data_engine_.get(), config_.chunk_size);
  // Slice retains opaque deferred cleanup records, but cannot physically
  // delete objects until #273 proves private-index reachability safety.
  return Status::OK();
}

Status VolumeImpl::CreateFrom(const config::ConfigCenter &config) {
  metadata::ChunkType chunk_type;
  auto status = metadata::ParseChunkType(config.chunk_type(), &chunk_type);
  if (!status.ok()) {
    return status;
  }
  return CreateFrom(FormatOptions{
      .name = config.volume(),
      .meta_url = config.meta_url(),
      .bucket = config.bucket_url(),
      .region = config.storage_region(),
      .chunk_size = config.chunk_size(),
      .chunk_type = chunk_type,
      .enable_posix_acl = config.enable_posix_acl(),
  });
}

Status VolumeImpl::CreateFrom(const FormatOptions &options) {
  utils::ExpectInThreadDomain();
  config_.name = options.name;
  config_.bucket = options.bucket;
  if (!config_.bucket.empty()) {
    utils::StorageUrl url;
    if (!utils::StorageUrl::Parse(config_.bucket, &url)) {
      return Status::InvalidArgument("invalid bucket URL: " + config_.bucket);
    }
    config_.storage = std::move(url.scheme);
  }
  config_.region = options.region;
  if (config_.region.empty()) {
    config_.region = "auto";
  }
  config_.chunk_size = options.chunk_size;
  config_.chunk_type = options.chunk_type;
  config_.enable_posix_acl = options.enable_posix_acl;

  if (config_.chunk_type == metadata::ChunkType::kCow) {
    return Status::NotSupported("cow Chunk mechanism is retired; format a new chunk_slice volume instead");
  }
  if (config_.chunk_type != metadata::ChunkType::kChunkSlice) {
    return Status::NotSupported("selected chunk type is not implemented");
  }

  auto status = CreateMetaEngine(options.meta_url, config_.name, &meta_engine_);
  if (!status.ok()) {
    return status;
  }
  status = meta_engine_->Initialize();
  if (!status.ok()) {
    return status;
  }
  return meta_engine_->FormatVolume(config_);
}

Status VolumeImpl::LoadFrom(const config::ConfigCenter &config) {
  return LoadFrom(MountOptions{
      .name = config.volume(),
      .meta_url = config.meta_url(),
      .storage_thread_count = static_cast<size_t>(std::max(1, config.storage_thread_count())),
  });
}

Status VolumeImpl::LoadFrom(const MountOptions &options) {
  utils::ExpectInThreadDomain();
  config_.name = options.name;

  auto status = CreateMetaEngine(options.meta_url, config_.name, &meta_engine_);
  if (!status.ok()) {
    return status;
  }
  status = meta_engine_->Initialize();
  if (!status.ok()) {
    return status;
  }
  status = meta_engine_->LoadVolume(&config_);
  if (!status.ok()) {
    return status;
  }
  if (config_.chunk_type == metadata::ChunkType::kCow) {
    return Status::NotSupported(
        "persisted cow Chunk mechanism is retired; explicit offline migration or "
        "reprovisioning as chunk_slice is required");
  }

  status = ComposeChunkMetadata();
  if (!status.ok()) {
    return status;
  }

  if (!config_.storage.empty()) {
    status = CreateDataEngine(config_.storage,
                              swordfs::storage::DataEngineOptions{
                                  .location = config_.bucket,
                                  .region = config_.region,
                                  .worker_count = options.storage_thread_count,
                              },
                              &data_engine_);
    if (!status.ok()) {
      return status;
    }
    status = data_engine_->Initialize();
    if (!status.ok()) {
      return status;
    }
  }

  return ComposeChunkRuntime();
}

void VolumeImpl::Shutdown() {
  utils::ExpectInThreadDomain();
  chunk_factory_.reset();
  data_engine_.reset();
  meta_engine_.reset();
  cleanup_work_encoder_.reset();
  chunk_metadata_.reset();
}

}  // namespace swordfs::volume
