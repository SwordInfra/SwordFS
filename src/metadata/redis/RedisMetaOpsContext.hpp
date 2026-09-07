// Copyright 2026 SwordFS Contributors.
// Licensed under the Apache License, Version 2.0.

#pragma once

namespace swordfs::metadata {

class RedisKvTxn;
class RedisMetaTxn;

// Transaction scope passed through RedisMetaOps. Callers can only use this
// context with RedisMetaOps APIs; the underlying Redis transaction remains
// private to the Redis metadata backend.
class RedisMetaOpsContext {
 private:
  friend class RedisMetaTxn;

  explicit RedisMetaOpsContext(RedisKvTxn &txn) : txn_(txn) {
  }

 private:
  RedisKvTxn &txn_;
};

}  // namespace swordfs::metadata
