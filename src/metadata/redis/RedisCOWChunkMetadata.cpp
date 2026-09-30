// Copyright 2026 SwordFS Contributors.
// Licensed under the Apache License, Version 2.0.

#include "metadata/redis/RedisCOWChunkMetadata.hpp"

#include <optional>
#include <string>

#include "metadata/redis/RedisBackendContext.hpp"
#include "metadata/redis/RedisKvTxn.hpp"
#include "metadata/redis/RedisMetaClient.hpp"
#include "metadata/types/BufCodec.hpp"
#include "utils/BlockingExecutor.hpp"
#include "utils/ExecutionDomain.hpp"

namespace swordfs::metadata {
namespace {

std::string EncodeHead(const cow::COWChunkHead &head) {
  BufEncoder encoder;
  encoder.U64(head.revision.Value());
  encoder.U64(head.size);
  std::string encoded;
  encoder.Finish(&encoded);
  return encoded;
}

utils::Status DecodeHead(std::string_view encoded, cow::COWChunkHead *out) {
  BufDecoder decoder(encoded);
  uint64_t revision = 0;
  uint64_t size = 0;
  decoder.U64(&revision);
  decoder.U64(&size);
  cow::COWChunkHead head{cow::COWChunkRevision(revision), size};
  if (!decoder.Done() || !cow::internal::IsValidHead(head)) {
    return utils::Status::Malformed("invalid Redis COW chunk head");
  }
  *out = head;
  return utils::Status::OK();
}

}  // namespace

utils::Status RedisCOWChunkMetadata::AllocateChunkID(ChunkID *out) {
  utils::ExpectInFiberDomain();
  if (out == nullptr) {
    return utils::Status::InvalidArgument("ChunkID output is null");
  }
  uint64_t value = 0;
  auto status =
      backend_->executor().RunFromFiber([&] { return backend_->client().IncrNonNegative(key_.NextChunkID(), &value); });
  if (!status.ok()) {
    return status;
  }
  *out = ChunkID(value);
  return utils::Status::OK();
}

utils::Status RedisCOWChunkMetadata::AllocateRevision(ChunkID chunk_id, cow::COWChunkRevision *out) {
  utils::ExpectInFiberDomain();
  if (!cow::internal::IsValidChunkID(chunk_id) || out == nullptr) {
    return utils::Status::InvalidArgument("invalid COW revision allocation request");
  }
  uint64_t value = 0;
  auto status = backend_->executor().RunFromFiber(
      [&] { return backend_->client().IncrNonNegative(RevisionKey(chunk_id), &value); });
  if (!status.ok()) {
    return status;
  }
  *out = cow::COWChunkRevision(value);
  return utils::Status::OK();
}

utils::Status RedisCOWChunkMetadata::GetHead(ChunkID chunk_id, cow::COWChunkHead *out) {
  utils::ExpectInFiberDomain();
  if (!cow::internal::IsValidChunkID(chunk_id) || out == nullptr) {
    return utils::Status::InvalidArgument("invalid COW head read request");
  }
  std::string encoded;
  auto status = backend_->executor().RunFromFiber([&] { return backend_->client().Get(HeadKey(chunk_id), &encoded); });
  if (!status.ok()) {
    return status;
  }
  return DecodeHead(encoded, out);
}

utils::Status RedisCOWChunkMetadata::CompareExchangeHead(ChunkID chunk_id,
                                                         const std::optional<cow::COWChunkHead> &expected,
                                                         const cow::COWChunkHead &replacement) {
  utils::ExpectInFiberDomain();
  if (!cow::internal::IsValidChunkID(chunk_id)) {
    return utils::Status::InvalidArgument("invalid COW ChunkID");
  }
  auto status = cow::internal::ValidateHeadTransition(expected, replacement);
  if (!status.ok()) {
    return status;
  }

  const auto key = HeadKey(chunk_id);
  const auto replacement_value = EncodeHead(replacement);
  return backend_->executor().RunFromFiber([&] {
    return backend_->client().Transact([&](RedisKvTxn &txn) {
      std::string current_value;
      auto txn_status = txn.Get(key, &current_value);
      if (txn_status.IsNotFound()) {
        if (expected.has_value()) {
          return utils::Status::NotFound("COW chunk head not found");
        }
        return txn.Set(key, replacement_value);
      }
      if (!txn_status.ok()) {
        return txn_status;
      }

      cow::COWChunkHead current;
      txn_status = DecodeHead(current_value, &current);
      if (!txn_status.ok()) {
        return txn_status;
      }
      if (!expected.has_value() || current != *expected) {
        return utils::Status::AlreadyExists("COW chunk head changed before publication");
      }
      return txn.Set(key, replacement_value);
    });
  });
}

utils::Status RedisCOWChunkMetadata::EraseHead(ChunkID chunk_id, const cow::COWChunkHead &expected) {
  utils::ExpectInFiberDomain();
  if (!cow::internal::IsValidChunkID(chunk_id) || !cow::internal::IsValidHead(expected)) {
    return utils::Status::InvalidArgument("invalid COW head erase request");
  }

  const auto key = HeadKey(chunk_id);
  return backend_->executor().RunFromFiber([&] {
    return backend_->client().Transact([&](RedisKvTxn &txn) {
      std::string current_value;
      auto txn_status = txn.Get(key, &current_value);
      if (!txn_status.ok()) {
        return txn_status;
      }
      cow::COWChunkHead current;
      txn_status = DecodeHead(current_value, &current);
      if (!txn_status.ok()) {
        return txn_status;
      }
      if (current != expected) {
        return utils::Status::AlreadyExists("COW chunk head changed before erase");
      }
      return txn.Del(key);
    });
  });
}

std::string RedisCOWChunkMetadata::HeadKey(ChunkID chunk_id) const {
  return key_.PrivateChunkIndex(ChunkType::kCow, "head:" + std::to_string(chunk_id.Value()));
}

std::string RedisCOWChunkMetadata::RevisionKey(ChunkID chunk_id) const {
  return key_.PrivateChunkIndex(ChunkType::kCow, "revision:" + std::to_string(chunk_id.Value()));
}

}  // namespace swordfs::metadata
