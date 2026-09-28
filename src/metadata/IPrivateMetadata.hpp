// Copyright 2026 SwordFS Contributors.
// Licensed under the Apache License, Version 2.0.

#pragma once

#include <cstdint>
#include <limits>
#include <memory>
#include <type_traits>
#include <typeinfo>

#include "metadata/types/Volume.hpp"
#include "utils/Status.hpp"

namespace swordfs::metadata {

// Redis INCR is signed 64-bit. Keeping the backend-neutral contract inside
// this range lets Memory and Redis expose identical private identity semantics.
constexpr uint64_t kMaxPrivateSequenceValue = static_cast<uint64_t>(std::numeric_limits<int64_t>::max());

// A mechanism owns the meaning of each stable sequence id. Mechanism code
// names the sequence with a compile-time tag and wraps the returned scalar in
// its own strong ID type; arbitrary runtime string sequence names are never
// part of the private metadata contract. Encoding the mechanism in the tag
// prevents a typed identity allocator from being used through the wrong
// mechanism-scoped store.
template <ChunkOverwriteMechanism Mechanism, uint32_t StableId>
struct PrivateSequenceTag {
  static_assert(Mechanism == ChunkOverwriteMechanism::kWholeObject ||
                    Mechanism == ChunkOverwriteMechanism::kChunkSlice ||
                    Mechanism == ChunkOverwriteMechanism::kRedisCache,
                "private sequence requires a known chunk overwrite mechanism");
  static_assert(StableId != 0, "private sequence id 0 is reserved");
  static constexpr ChunkOverwriteMechanism kMechanism = Mechanism;
  static constexpr uint32_t kStableId = StableId;
};

namespace internal {

// Allocate the next portable private sequence value in-place. Keeping the
// checked state transition here lets backends reuse one exhaustion rule
// without exposing a production setter merely to test the boundary.
utils::Status AllocatePrivateSequenceValue(uint64_t *current, uint64_t *value);

}  // namespace internal

// Runtime capability shared between the selected mechanism and its metadata
// backend. This interface deliberately contains no generic record read/write
// methods: concrete mechanisms add typed stores beside their record types.
class IMechanismPrivateStore {
 public:
  virtual ~IMechanismPrivateStore() = default;

  virtual ChunkOverwriteMechanism mechanism() const = 0;

  template <ChunkOverwriteMechanism Mechanism, uint32_t StableId>
  utils::Status AllocateSequence(PrivateSequenceTag<Mechanism, StableId>, uint64_t *value) {
    if (value == nullptr) {
      return utils::Status::InvalidArgument("private sequence output is null");
    }
    if (mechanism() != Mechanism) {
      return utils::Status::InvalidArgument("private sequence mechanism does not match store");
    }
    return AllocateSequenceImpl(StableId, value);
  }

 private:
  virtual utils::Status AllocateSequenceImpl(uint32_t stable_id, uint64_t *value) = 0;
};

// Backend transaction adapter root. Mechanism-owned typed transaction
// interfaces derive from this marker. Memory adapters override Commit() to
// publish their staged typed changes only after the enclosing metadata
// transaction succeeds; Redis adapters normally queue writes directly into
// RedisKvTxn and can use the default no-op commit hook.
//
// Commit() is intentionally not part of the mechanism-facing semantic API.
// Only the transaction capability context may finalize the adapter.
class IMechanismPrivateTxn {
 public:
  virtual ~IMechanismPrivateTxn() = default;

 private:
  friend class MechanismPrivateTxnContext;
  virtual void Commit() noexcept {
  }
};

// Transaction-local type-safe binding point. A backend-specific mechanism
// adapter binds exactly one typed transaction interface here; common metadata
// code never learns that interface's record layout or Redis representation.
// The backend transaction owns the bound adapter and must keep it alive until
// this context is finalized; binding a callback-local temporary would leave
// the commit hook dangling after the callback returns.
class MechanismPrivateTxnContext {
 public:
  explicit MechanismPrivateTxnContext(ChunkOverwriteMechanism mechanism) : mechanism_(mechanism) {
  }

  ChunkOverwriteMechanism mechanism() const {
    return mechanism_;
  }

  template <typename Capability>
  utils::Status Bind(Capability *capability) {
    static_assert(std::is_base_of_v<IMechanismPrivateTxn, Capability>,
                  "private transaction capability must derive from IMechanismPrivateTxn");
    if (capability == nullptr) {
      return utils::Status::InvalidArgument("private transaction capability is null");
    }
    if (Capability::kMechanism != mechanism_) {
      return utils::Status::InvalidArgument("private transaction capability mechanism does not match transaction");
    }
    if (capability_ != nullptr) {
      return utils::Status::AlreadyExists("private transaction capability is already bound");
    }
    capability_ = capability;
    transaction_ = capability;
    capability_type_ = &typeid(Capability);
    return utils::Status::OK();
  }

  template <typename Capability>
  Capability *Get() const {
    if (Capability::kMechanism != mechanism_ || capability_type_ == nullptr ||
        *capability_type_ != typeid(Capability)) {
      return nullptr;
    }
    return static_cast<Capability *>(capability_);
  }

 private:
  friend class MemMetaTxn;

  void Commit() noexcept {
    if (transaction_ != nullptr) {
      transaction_->Commit();
    }
  }

  ChunkOverwriteMechanism mechanism_;
  void *capability_ = nullptr;
  IMechanismPrivateTxn *transaction_ = nullptr;
  const std::type_info *capability_type_ = nullptr;
};

using MechanismPrivateStorePtr = std::shared_ptr<IMechanismPrivateStore>;

}  // namespace swordfs::metadata
