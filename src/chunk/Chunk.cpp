// Copyright 2026 SwordFS Contributors.
// Licensed under the Apache License, Version 2.0.

#include "chunk/Chunk.hpp"

#include <folly/Random.h>
#include <folly/io/IOBuf.h>
#include <folly/logging/xlog.h>

#include <algorithm>
#include <cstring>

#include "metadata/IMetaEngine.hpp"
#include "storage/IDataEngine.hpp"
#include "utils/Logging.hpp"
#include "volume/VolumeImpl.hpp"

namespace swordfs::chunk {

Chunk::Chunk(metadata::InodeID ino, metadata::ChunkIndex index)
    : ino_(ino),
      max_chunk_size_(volume::VolumeImpl::Instance().chunk_size()),
      index_(index),
      data_(volume::VolumeImpl::Instance().data_engine()),
      meta_(volume::VolumeImpl::Instance().meta_engine()) {
}

utils::Status Chunk::Initialize() {
  metadata::SwordFsChunk chunk;
  auto status = meta_->FindChunk(ino_, index_, &chunk);
  if (status.ok()) {
    state_ = State::kFlushed;
    published_chunk_ = std::move(chunk);
    return Status::OK();
  } else if (status.IsNotFound()) {
    // create a chunk but not commit to metadata so only the local mount knows it.
    state_ = State::kWriting;
    wb_ = std::make_unique<WriteBuf>(max_chunk_size_);
    return Status::OK();
  }
  return status;
}

utils::Status Chunk::Write(off_t write_offset, const folly::IOBuf &data) {
  if (IsFlushed()) {
    auto status = HydrateForWrite();
    if (!status.ok()) {
      return status;
    }
  }
  if (!IsWriting()) {
    return utils::Status::InvalidArgument("Chunk::Write: chunk is sealed");
  }
  auto status = wb_->Write(write_offset - StartOffset(), data);
  if (!status.ok()) {
    SWORDFS_LOG_ERROR << "Chunk::Write FAILED: ino=" << ino_ << " index=" << index_ << " write_offset=" << write_offset
                      << " data_size=" << data.length() << " — " << status.message();
  }
  return status;
}

utils::Status Chunk::Read(off_t off, size_t len, folly::IOBuf *out) const {
  if (out->tailroom() < len) {
    return utils::Status::InvalidArgument("Chunk::Read: output buffer too small");
  }
  if (IsFlushed()) {
    return data_->Get(PublishedChunk().key, off, len, out);
  }
  return wb_->CopyOut(off, len, out);
}

void Chunk::Seal() {
  state_ = State::kSealed;
}

void Chunk::Truncate(size_t size) {
  if (IsFlushed()) {
    if (published_chunk_) {
      published_chunk_->size = std::min<uint64_t>(published_chunk_->size, size);
    }
    return;
  }
  if (wb_) {
    wb_->Truncate(size);
  }
  if (published_chunk_) {
    // A rewrite remembers the old descriptor as its CAS expectation. Metadata
    // truncate clamps that descriptor too, so keep the local expectation in
    // lockstep for a later flush/retry.
    published_chunk_->size = std::min<uint64_t>(published_chunk_->size, size);
  }
}

std::vector<std::string> Chunk::ObjectKeysForCleanup() const {
  std::vector<std::string> keys;
  if (published_chunk_ && !published_chunk_->key.empty()) {
    keys.push_back(published_chunk_->key);
  } else if (wb_) {
    // A brand-new chunk may have uploaded its deterministic first-version
    // key before metadata publication failed.
    keys.push_back(FormatChunkKey(ino_, index_));
  }
  if (!pending_key_.empty() && (keys.empty() || keys.front() != pending_key_)) {
    keys.push_back(pending_key_);
  }
  return keys;
}

utils::Status Chunk::Flush() {
  if (IsFlushed() || !wb_ || wb_->size() == 0) {
    return utils::Status::OK();
  }

  const bool retrying = !IsWriting();
  if (IsWriting()) {
    Seal();
  }

  const auto chunk_meta = BuildMeta();

  if (published_chunk_) {
    // A rewrite is copy-on-write: the old object remains authoritative until
    // metadata atomically selects |chunk_meta|. On an ambiguous retry, first
    // resolve which descriptor metadata currently contains.
    if (retrying) {
      metadata::SwordFsChunk current;
      auto status = meta_->FindChunk(ino_, index_, &current);
      if (!status.ok()) {
        return status;
      }
      if (current == chunk_meta) {
        // The descriptor may have been applied by a Redis MULTI/EXEC whose
        // later inode update failed. Re-enter the idempotent metadata
        // mutation so size/timestamps/mode side effects are reconciled before
        // reporting the ambiguous rewrite as committed.
        status = meta_->ReplaceChunk(ino_, *published_chunk_, chunk_meta);
        if (!status.ok()) {
          return status;
        }
        CompletePublication(chunk_meta);
        return utils::Status::OK();
      }
      if (!(current == *published_chunk_)) {
        // A partial metadata commit can leave the pending object selected by
        // metadata with a descriptor that differs only in size. Never delete
        // an object that the authoritative descriptor still references.
        if (current.key != chunk_meta.key) {
          auto delete_status = data_->Delete(chunk_meta.key);
          if (!delete_status.ok()) {
            SWORDFS_LOG_ERROR << "Chunk::Flush retry conflict cleanup FAILED: key=" << chunk_meta.key << " — "
                              << delete_status.message();
          }
        }
        return utils::Status::AlreadyExists("Chunk::Flush: published chunk changed before rewrite at index " +
                                            std::to_string(index_));
      }
    }

    auto data = wb_->CloneBuf();
    auto status = data_->Put(chunk_meta.key, std::move(data));
    if (!status.ok()) {
      SWORDFS_LOG_ERROR << "Chunk::Flush rewrite Put FAILED: ino=" << ino_ << " chunk=" << index_
                        << " size=" << chunk_meta.size << " — " << status.message();
      return status;
    }

    status = meta_->ReplaceChunk(ino_, *published_chunk_, chunk_meta);
    if (!status.ok()) {
      if (status.IsAlreadyExists()) {
        auto delete_status = data_->Delete(chunk_meta.key);
        if (!delete_status.ok()) {
          SWORDFS_LOG_ERROR << "Chunk::Flush conflict cleanup FAILED: key=" << chunk_meta.key << " — "
                            << delete_status.message();
        }
      }
      SWORDFS_LOG_ERROR << "Chunk::Flush ReplaceChunk FAILED: ino=" << ino_ << " chunk=" << index_
                        << " size=" << chunk_meta.size << " — " << status.message();
      return status;
    }

    CompletePublication(chunk_meta);
    return utils::Status::OK();
  }

  // A failed/ambiguous publication leaves the chunk sealed. Before retrying
  // the object Put, resolve metadata first so a committed previous attempt is
  // completed idempotently and a conflicting descriptor cannot be overwritten.
  if (retrying) {
    metadata::SwordFsChunk existing;
    auto status = meta_->FindChunk(ino_, index_, &existing);
    if (status.ok()) {
      if (!(existing == chunk_meta)) {
        return utils::Status::AlreadyExists("Chunk::Flush: conflicting published chunk at index " +
                                            std::to_string(index_));
      }
      status = meta_->PublishChunk(ino_, chunk_meta);
      if (!status.ok()) {
        return status;
      }
      CompletePublication(chunk_meta);
      return utils::Status::OK();
    }
    if (!status.IsNotFound()) {
      return status;
    }
  }

  auto data = wb_->CloneBuf();
  auto status = data_->Put(chunk_meta.key, std::move(data));
  if (!status.ok()) {
    SWORDFS_LOG_ERROR << "Chunk::Flush FAILED: ino=" << ino_ << " chunk=" << index_ << " size=" << chunk_meta.size
                      << " — " << status.message();
    return status;
  }

  status = meta_->PublishChunk(ino_, chunk_meta);
  if (!status.ok()) {
    SWORDFS_LOG_ERROR << "Chunk::Flush FAILED: PublishChunk ino=" << ino_ << " chunk=" << index_
                      << " size=" << chunk_meta.size << " — " << status.message();
    return status;
  }

  CompletePublication(chunk_meta);

  SWORDFS_LOG_DEBUG << "Flush uploaded: ino=" << ino_ << " chunk=" << index_ << " size=" << chunk_meta.size;
  return utils::Status::OK();
}

metadata::SwordFsChunk Chunk::BuildMeta() const {
  metadata::SwordFsChunk chunk;
  chunk.index = index_;
  chunk.start_offset = static_cast<uint64_t>(StartOffset());
  chunk.key = pending_key_.empty() ? FormatChunkKey(ino_, index_) : pending_key_;
  chunk.size = wb_ ? wb_->size() : 0;
  return chunk;
}

const metadata::SwordFsChunk &Chunk::PublishedChunk() const {
  CHECK(published_chunk_.has_value());
  return *published_chunk_;
}

utils::Status Chunk::HydrateForWrite() {
  const auto &published = PublishedChunk();
  if (published.size > max_chunk_size_) {
    return utils::Status::Malformed("Chunk::HydrateForWrite: published chunk exceeds configured chunk size");
  }

  auto wb = std::make_unique<WriteBuf>(max_chunk_size_);
  if (published.size > 0) {
    auto persisted = folly::IOBuf::create(published.size);
    auto status = data_->Get(published.key, 0, published.size, persisted.get());
    if (!status.ok()) {
      return status;
    }
    if (persisted->length() != published.size) {
      return utils::Status::IOError("Chunk::HydrateForWrite: persisted object is shorter than metadata descriptor");
    }
    status = wb->Write(0, *persisted);
    if (!status.ok()) {
      return status;
    }
  }

  wb_ = std::move(wb);
  pending_key_ = NewRewriteKey();
  state_ = State::kWriting;
  return utils::Status::OK();
}

void Chunk::CompletePublication(const metadata::SwordFsChunk &chunk) {
  std::string old_key;
  if (published_chunk_) {
    old_key = published_chunk_->key;
  }

  published_chunk_ = chunk;
  pending_key_.clear();
  state_ = State::kFlushed;
  wb_.reset();

  if (!old_key.empty() && old_key != chunk.key) {
    auto status = data_->Delete(old_key);
    if (!status.ok()) {
      SWORDFS_LOG_ERROR << "Chunk::CompletePublication old-object cleanup FAILED: key=" << old_key << " — "
                        << status.message();
    }
  }
}

std::string Chunk::NewRewriteKey() const {
  return FormatChunkKey(ino_, index_) + "/r" + std::to_string(folly::Random::secureRand64()) + "-" +
         std::to_string(folly::Random::secureRand64());
}

}  // namespace swordfs::chunk
