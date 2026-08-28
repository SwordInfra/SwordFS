// Copyright 2026 SwordFS Contributors.
// Licensed under the Apache License, Version 2.0.

#include "metadata/redis/RedisMetaImpl.hpp"

#include <dirent.h>
#include <folly/fibers/FiberManagerInternal.h>
#include <sw/redis++/redis++.h>
#include <sys/stat.h>

#include <algorithm>
#include <charconv>
#include <chrono>
#include <exception>
#include <mutex>
#include <optional>
#include <stdexcept>
#include <unordered_set>
#include <utility>
#include <vector>

#include "metadata/MetaEngineRegistry.hpp"
#include "metadata/Utils.hpp"
#include "metadata/redis/RedisKey.hpp"
#include "metadata/redis/RedisMetaClient.hpp"
#include "metadata/types/BufCodec.hpp"
#include "metadata/types/Chunk.hpp"
#include "metadata/types/Common.hpp"
#include "metadata/types/Entry.hpp"
#include "metadata/types/Inode.hpp"
#include "metadata/types/Volume.hpp"
#include "utils/Context.hpp"

namespace swordfs::metadata {
namespace {
using namespace redis;

utils::Status ParseDirValue(std::string_view value, uint32_t *type, InodeID *ino);

class RedisDirIterator final : public IDirIterator {
 public:
  RedisDirIterator(std::shared_ptr<RedisMetaClient> client, std::string key, InodeID ino, InodeID parent_ino)
      : client_(std::move(client)), key_(std::move(key)), ino_(ino), parent_ino_(parent_ino) {
  }

  Status Peek(uint64_t offset, SwordFsEntry *entry, uint64_t *next_offset, bool *end) override {
    if (entry == nullptr || next_offset == nullptr || end == nullptr) {
      return Status::InvalidArgument("directory iterator output is null");
    }
    std::lock_guard lock(mutex_);
    const auto saved_cursor = cursor_;
    const auto saved_logical_offset = logical_offset_;
    const auto saved_exhausted = exhausted_;
    const auto saved_pending = pending_;
    std::vector<SwordFsEntry> entries;
    uint64_t peek_next = offset;
    bool peek_end = false;
    auto status = ReadLocked(offset, 1, &entries, &peek_next, &peek_end);
    cursor_ = saved_cursor;
    logical_offset_ = saved_logical_offset;
    exhausted_ = saved_exhausted;
    pending_ = saved_pending;
    if (!status.ok()) {
      return status;
    }
    if (entries.empty()) {
      *next_offset = peek_next;
      *end = peek_end;
      return Status::NotFound("directory end");
    }
    *entry = entries.front();
    *next_offset = peek_next;
    *end = peek_end;
    return Status::OK();
  }

  Status Read(uint64_t offset, size_t max_entries, std::vector<SwordFsEntry> *entries, uint64_t *next_offset,
              bool *end) override {
    std::lock_guard lock(mutex_);
    return ReadLocked(offset, max_entries, entries, next_offset, end);
  }

 private:
  Status ReadLocked(uint64_t offset, size_t max_entries, std::vector<SwordFsEntry> *entries, uint64_t *next_offset,
                    bool *end) {
    if (entries == nullptr || next_offset == nullptr || end == nullptr) {
      return Status::InvalidArgument("directory iterator output is null");
    }
    if (max_entries == 0) {
      entries->clear();
      *next_offset = offset;
      *end = false;
      return Status::OK();
    }

    if (offset != logical_offset_) {
      cursor_ = 0;
      logical_offset_ = 2;
      exhausted_ = false;
      pending_.clear();
    }

    entries->clear();

    // The synthetic entries are stable logical positions 0 and 1. Seeking
    // into them always restarts the backend cursor because HSCAN only has an
    // opaque backend cursor, not a FUSE-compatible directory cookie.
    if (offset < 2) {
      if (offset == 0 && entries->size() < max_entries) {
        entries->push_back({".", DT_DIR, ino_});
      }
      if (offset <= 1 && entries->size() < max_entries) {
        entries->push_back({"..", DT_DIR, parent_ino_});
      }
      logical_offset_ = offset + entries->size();
      if (logical_offset_ < 2) {
        logical_offset_ = 2;
      }
    }

    while (entries->size() < max_entries) {
      if (pending_.empty()) {
        if (exhausted_) {
          break;
        }
        std::vector<std::pair<std::string, std::string>> values;
        uint64_t next_cursor = cursor_;
        auto status = Scan(&values, &next_cursor);
        if (!status.ok()) {
          return status;
        }
        cursor_ = next_cursor;
        exhausted_ = cursor_ == 0;
        for (const auto &[name, value] : values) {
          uint32_t type;
          InodeID child_ino;
          status = ParseDirValue(value, &type, &child_ino);
          if (!status.ok()) {
            return status;
          }
          pending_.push_back({name, type, child_ino});
        }
        if (values.empty() && exhausted_) {
          break;
        }
      }

      const SwordFsEntry entry = std::move(pending_.front());
      pending_.erase(pending_.begin());
      if (logical_offset_ < offset) {
        ++logical_offset_;
        continue;
      }
      entries->push_back(entry);
      ++logical_offset_;
    }

    if (logical_offset_ < offset) {
      *next_offset = offset;
      *end = true;
      return Status::OK();
    }
    *next_offset = logical_offset_;
    *end = exhausted_;
    return Status::OK();
  }

 private:
  Status Scan(std::vector<std::pair<std::string, std::string>> *values, uint64_t *next_cursor) {
    return client_->HScan(key_, cursor_, 128, values, next_cursor);
  }

  mutable std::mutex mutex_;
  std::shared_ptr<RedisMetaClient> client_;
  std::string key_;
  InodeID ino_;
  InodeID parent_ino_;
  uint64_t cursor_ = 0;
  uint64_t logical_offset_ = 2;
  bool exhausted_ = false;
  std::vector<SwordFsEntry> pending_;
};
utils::Status CreateRedisMetaEngine(std::string_view meta_url, std::string_view volume_name,
                                    std::unique_ptr<IMetaEngine> *out) {
  if (out == nullptr) {
    return utils::Status::InvalidArgument("metadata engine output is null");
  }

  RedisMetaConfig config;
  auto status = ParseRedisMetaUrl(meta_url, &config);
  if (!status.ok()) {
    return status;
  }

  try {
    auto redis = std::make_unique<RedisMetaImpl>(config, volume_name);
    *out = std::move(redis);
    return utils::Status::OK();
  } catch (const std::invalid_argument &error) {
    return utils::Status::InvalidArgument(error.what());
  } catch (const std::exception &error) {
    return utils::Status::IOError("Redis metadata initialization failed: " + std::string(error.what()));
  }
}

RegisterMetaEngine kRedisMetaEngine{"redis", CreateRedisMetaEngine};

std::string SerializeDirValue(uint32_t type, InodeID ino) {
  BufEncoder encoder;
  encoder.U32(type);
  encoder.U64(ino);
  std::string value;
  encoder.Finish(&value);
  return value;
}

utils::Status ParseDirValue(std::string_view value, uint32_t *type, InodeID *ino) {
  if (type == nullptr || ino == nullptr) {
    return utils::Status::InvalidArgument("directory entry output is null");
  }
  BufDecoder decoder(value);
  if (!decoder.U32(type) || !decoder.U64(ino) || *ino == 0 || !decoder.Done()) {
    return utils::Status::Malformed("malformed Redis directory entry");
  }
  return utils::Status::OK();
}

int64_t NowSeconds() {
  return std::chrono::duration_cast<std::chrono::seconds>(std::chrono::system_clock::now().time_since_epoch()).count();
}

int64_t NowNanoseconds() {
  return std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::system_clock::now().time_since_epoch())
             .count() %
         1000000000;
}

void Touch(SwordFsInode *inode, bool atime, bool mtime, bool ctime) {
  const int64_t seconds = NowSeconds();
  const int64_t nanoseconds = NowNanoseconds();
  if (atime) {
    inode->attr.atime = seconds;
    inode->attr.atime_nsec = nanoseconds;
  }
  if (mtime) {
    inode->attr.mtime = seconds;
    inode->attr.mtime_nsec = nanoseconds;
  }
  if (ctime) {
    inode->attr.ctime = seconds;
    inode->attr.ctime_nsec = nanoseconds;
  }
}

utils::Status ParseInode(std::string_view value, SwordFsInode *inode) {
  if (inode == nullptr) {
    return utils::Status::InvalidArgument("inode output is null");
  }
  return inode->ParseFrom(value);
}

utils::Status SerializeInode(const SwordFsInode &inode, std::string *value) {
  return inode.SerializeTo(value);
}

}  // namespace

RedisMetaImpl::RedisMetaImpl(const RedisMetaConfig &config, std::string_view volume_name)
    : client_(std::make_shared<RedisMetaClient>(config)), key_(config.db, volume_name) {
}

RedisMetaImpl::~RedisMetaImpl() = default;

utils::Status RedisMetaImpl::Initialize() {
  try {
    return client_->Ping();
  } catch (const std::exception &error) {
    return utils::Status::IOError("Redis metadata initialization failed: " + std::string(error.what()));
  }
}

utils::Status RedisMetaImpl::FormatVolume(const SwordFsVolume &config) {
  SwordFsInode root;
  root.ino = kRootInodeId;
  root.parent_ino = kRootInodeId;
  root.attr = SwordFsAttr(kRootInodeId, S_IFDIR | 0777);
  std::string root_value;
  auto status = root.SerializeTo(&root_value);
  if (!status.ok()) {
    return status;
  }

  return client_->Transact([&](RedisMetaTxn &txn) {
    std::string existing;
    auto status = txn.Get(key_.Format(), &existing);
    if (status.ok()) {
      return utils::Status::AlreadyExists("Redis metadata volume is already formatted");
    }
    if (!status.IsNotFound()) {
      return status;
    }
    status = txn.Set(key_.Format(), config.SerializeTo());
    if (!status.ok()) {
      return status;
    }
    status = txn.Set(key_.NextIno(), std::to_string(kRootInodeId + 1));
    if (!status.ok()) {
      return status;
    }
    status = txn.Set(key_.InodeCount(), "1");
    if (!status.ok()) {
      return status;
    }
    return txn.Set(key_.Inode(kRootInodeId), root_value);
  });
}

utils::Status RedisMetaImpl::LoadVolume(SwordFsVolume *config) {
  if (config == nullptr) {
    return utils::Status::InvalidArgument("Redis volume config output is null");
  }

  std::string value;
  auto status = client_->Get(key_.Format(), &value);
  if (!status.ok()) {
    return status;
  }

  std::string root_value;
  status = client_->Get(key_.Inode(kRootInodeId), &root_value);
  if (!status.ok()) {
    return status;
  }

  SwordFsInode root;
  status = root.ParseFrom(root_value);
  if (!status.ok()) {
    return status;
  }
  if (root.ino != kRootInodeId || !root.IsDir()) {
    return utils::Status::InvalidArgument("Redis metadata root inode is invalid");
  }
  status = config->ParseFrom(value);
  if (!status.ok()) {
    return status;
  }
  return utils::Status::OK();
}

Limits RedisMetaImpl::GetLimits() const {
  return {.max_name_length = 255, .max_free_inodes = UINT64_MAX};
}

Status RedisMetaImpl::Lookup(InodeID parent_ino, std::string_view name, SwordFsInode *out) {
  if (out == nullptr) {
    return Status::InvalidArgument("Lookup output is null");
  }
  return client_->Transact([&](RedisMetaTxn &txn) {
    SwordFsInode parent;
    std::string value;
    auto status = txn.Get(key_.Inode(parent_ino), &value);
    if (!status.ok()) {
      return status;
    }
    status = ParseInode(value, &parent);
    if (!status.ok()) {
      return status;
    }
    if (!parent.IsDir()) {
      return Status::NotDirectory("parent is not a directory");
    }
    std::string entry_value;
    status = txn.HGet(key_.Directory(parent_ino), name, &entry_value);
    if (!status.ok()) {
      return status;
    }
    uint32_t type;
    InodeID child_ino;
    status = ParseDirValue(entry_value, &type, &child_ino);
    if (!status.ok()) {
      return status;
    }
    (void)type;
    status = txn.Get(key_.Inode(child_ino), &entry_value);
    if (!status.ok()) {
      return status;
    }
    return ParseInode(entry_value, out);
  });
}

Status RedisMetaImpl::GetInode(InodeID ino, SwordFsInode *out) {
  if (out == nullptr) {
    return Status::InvalidArgument("GetInode output is null");
  }
  return client_->Transact([&](RedisMetaTxn &txn) {
    std::string value;
    auto status = txn.Get(key_.Inode(ino), &value);
    if (!status.ok()) {
      return status;
    }
    return ParseInode(value, out);
  });
}

Status RedisMetaImpl::OpenDirIterator(InodeID ino, std::unique_ptr<IDirIterator> *out) {
  if (out == nullptr) {
    return Status::InvalidArgument("directory iterator output is null");
  }
  SwordFsInode dir;
  auto status = GetInode(ino, &dir);
  if (!status.ok()) {
    return status;
  }
  if (!dir.IsDir()) {
    return Status::NotDirectory("not a directory");
  }
  *out = std::make_unique<RedisDirIterator>(client_, key_.Directory(ino), ino, dir.parent_ino);
  return Status::OK();
}

Status RedisMetaImpl::ReadDir(InodeID ino, std::vector<SwordFsEntry> *entries) {
  if (entries == nullptr) {
    return Status::InvalidArgument("ReadDir output is null");
  }
  return client_->Transact([&](RedisMetaTxn &txn) {
    SwordFsInode dir;
    std::string value;
    auto status = txn.Get(key_.Inode(ino), &value);
    if (!status.ok()) {
      return status;
    }
    status = ParseInode(value, &dir);
    if (!status.ok()) {
      return status;
    }
    if (!dir.IsDir()) {
      return Status::NotDirectory("not a directory");
    }
    std::vector<std::pair<std::string, std::string>> values;
    status = txn.HGetAll(key_.Directory(ino), &values);
    if (!status.ok()) {
      return status;
    }
    entries->clear();
    entries->push_back({".", DT_DIR, ino});
    entries->push_back({"..", DT_DIR, dir.parent_ino});
    for (const auto &[name, entry_value] : values) {
      uint32_t type;
      InodeID child_ino;
      status = ParseDirValue(entry_value, &type, &child_ino);
      if (!status.ok()) {
        return status;
      }
      entries->push_back({name, type, child_ino});
    }
    Touch(&dir, true, false, false);
    status = SerializeInode(dir, &value);
    if (!status.ok()) {
      return status;
    }
    return txn.Set(key_.Inode(ino), value);
  });
}

Status RedisMetaImpl::Create(InodeID parent_ino, std::string_view name, uint32_t mode, SwordFsInode *out) {
  if (name.size() > 255) {
    return Status::NameTooLong("file name exceeds maximum length");
  }
  const auto ctx = folly::fibers::local<SwordFsContext>();
  InodeID child_ino;
  auto status = client_->Incr(key_.NextIno(), &child_ino);
  if (!status.ok()) {
    return status;
  }
  SwordFsInode child;
  status = client_->Transact([&](RedisMetaTxn &txn) {
    SwordFsInode parent;
    std::string value;
    auto status = txn.Get(key_.Inode(parent_ino), &value);
    if (!status.ok()) {
      return status;
    }
    status = ParseInode(value, &parent);
    if (!status.ok()) {
      return status;
    }
    if (!parent.IsDir()) {
      return Status::NotDirectory("parent is not a directory");
    }
    if (!parent.CheckAccess(ctx.uid, ctx.gid, W_OK | X_OK)) {
      return Status::Permission("access denied on parent");
    }
    std::string ignored;
    status = txn.HGet(key_.Directory(parent_ino), name, &ignored);
    if (status.ok()) {
      return Status::AlreadyExists("entry already exists");
    }
    if (!status.IsNotFound()) {
      return status;
    }
    SwordFsAttr attr(child_ino, S_IFREG | (mode & 0777u));
    attr.uid = ctx.uid;
    attr.gid = parent.attr.gid;
    child = SwordFsInode(child_ino, attr, parent_ino);
    Touch(&parent, false, true, true);
    std::string child_value;
    status = SerializeInode(child, &child_value);
    if (!status.ok()) {
      return status;
    }
    status = txn.Set(key_.Inode(child_ino), child_value);
    if (!status.ok()) {
      return status;
    }
    status = txn.HSet(key_.Directory(parent_ino), name, SerializeDirValue(DT_REG, child_ino));
    if (!status.ok()) {
      return status;
    }
    status = SerializeInode(parent, &value);
    if (!status.ok()) {
      return status;
    }
    status = txn.Set(key_.Inode(parent_ino), value);
    if (!status.ok()) {
      return status;
    }
    return txn.IncrBy(key_.InodeCount(), 1);
  });
  if (status.ok() && out) {
    *out = child;
  }
  return status;
}

Status RedisMetaImpl::MkDir(InodeID parent_ino, std::string_view name, uint32_t mode, SwordFsInode *out) {
  if (name.size() > 255) {
    return Status::NameTooLong("directory name exceeds maximum length");
  }
  const auto ctx = folly::fibers::local<SwordFsContext>();
  InodeID child_ino;
  auto status = client_->Incr(key_.NextIno(), &child_ino);
  if (!status.ok()) {
    return status;
  }
  SwordFsInode child;
  status = client_->Transact([&](RedisMetaTxn &txn) {
    SwordFsInode parent;
    std::string value;
    auto status = txn.Get(key_.Inode(parent_ino), &value);
    if (!status.ok()) {
      return status;
    }
    status = ParseInode(value, &parent);
    if (!status.ok()) {
      return status;
    }
    if (!parent.IsDir()) {
      return Status::NotDirectory("parent is not a directory");
    }
    if (!parent.CheckAccess(ctx.uid, ctx.gid, W_OK | X_OK)) {
      return Status::Permission("access denied on parent");
    }
    std::string ignored;
    status = txn.HGet(key_.Directory(parent_ino), name, &ignored);
    if (status.ok()) {
      return Status::AlreadyExists("entry already exists");
    }
    if (!status.IsNotFound()) {
      return status;
    }
    SwordFsAttr attr(child_ino, S_IFDIR | (mode & 0777u));
    attr.uid = ctx.uid;
    attr.gid = parent.attr.gid;
    child = SwordFsInode(child_ino, attr, parent_ino);
    parent.attr.nlink++;
    Touch(&parent, false, true, true);
    std::string child_value;
    status = SerializeInode(child, &child_value);
    if (!status.ok()) {
      return status;
    }
    status = txn.Set(key_.Inode(child_ino), child_value);
    if (!status.ok()) {
      return status;
    }
    status = txn.HSet(key_.Directory(parent_ino), name, SerializeDirValue(DT_DIR, child_ino));
    if (!status.ok()) {
      return status;
    }
    status = SerializeInode(parent, &value);
    if (!status.ok()) {
      return status;
    }
    status = txn.Set(key_.Inode(parent_ino), value);
    if (!status.ok()) {
      return status;
    }
    return txn.IncrBy(key_.InodeCount(), 1);
  });
  if (status.ok() && out) {
    *out = child;
  }
  return status;
}

Status RedisMetaImpl::Unlink(InodeID parent_ino, std::string_view name, uint64_t *post_nlink) {
  if (name == "." || name == "..") {
    return Status::InvalidArgument("cannot unlink . or ..");
  }
  const auto ctx = folly::fibers::local<SwordFsContext>();
  return client_->Transact([&](RedisMetaTxn &txn) {
    SwordFsInode parent;
    std::string value;
    auto status = txn.Get(key_.Inode(parent_ino), &value);
    if (!status.ok()) {
      return status;
    }
    status = ParseInode(value, &parent);
    if (!status.ok()) {
      return status;
    }
    if (!parent.IsDir()) {
      return Status::NotDirectory("parent is not a directory");
    }
    if (!parent.CheckAccess(ctx.uid, ctx.gid, W_OK | X_OK)) {
      return Status::Permission("access denied on parent");
    }
    std::string entry_value;
    status = txn.HGet(key_.Directory(parent_ino), name, &entry_value);
    if (!status.ok()) {
      return status;
    }
    uint32_t type;
    InodeID child_ino;
    status = ParseDirValue(entry_value, &type, &child_ino);
    if (!status.ok()) {
      return status;
    }
    SwordFsInode child;
    status = txn.Get(key_.Inode(child_ino), &value);
    if (!status.ok()) {
      return status;
    }
    status = ParseInode(value, &child);
    if (!status.ok()) {
      return status;
    }
    if (child.IsDir()) {
      return Status::InvalidArgument("cannot unlink directory");
    }
    if (!parent.CheckStickyDelete(ctx.uid, child)) {
      return Status::Permission("sticky bit denied");
    }
    child.attr.nlink--;
    Touch(&child, false, false, true);
    Touch(&parent, false, true, true);
    status = txn.HDel(key_.Directory(parent_ino), name);
    if (!status.ok()) {
      return status;
    }
    status = SerializeInode(parent, &value);
    if (!status.ok()) {
      return status;
    }
    status = txn.Set(key_.Inode(parent_ino), value);
    if (!status.ok()) {
      return status;
    }
    status = SerializeInode(child, &value);
    if (!status.ok()) {
      return status;
    }
    status = txn.Set(key_.Inode(child_ino), value);
    if (!status.ok()) {
      return status;
    }
    if (post_nlink) {
      *post_nlink = child.attr.nlink;
    }
    return Status::OK();
  });
}

Status RedisMetaImpl::RmDir(InodeID parent_ino, std::string_view name) {
  if (name == "." || name == "..") {
    return Status::InvalidArgument("cannot remove . or ..");
  }
  const auto ctx = folly::fibers::local<SwordFsContext>();
  return client_->Transact([&](RedisMetaTxn &txn) {
    SwordFsInode parent, child;
    std::string value, entry_value;
    auto status = txn.Get(key_.Inode(parent_ino), &value);
    if (!status.ok()) {
      return status;
    }
    status = ParseInode(value, &parent);
    if (!status.ok()) {
      return status;
    }
    if (!parent.IsDir()) {
      return Status::NotDirectory("parent is not a directory");
    }
    if (!parent.CheckAccess(ctx.uid, ctx.gid, W_OK | X_OK)) {
      return Status::Permission("access denied on parent");
    }
    status = txn.HGet(key_.Directory(parent_ino), name, &entry_value);
    if (!status.ok()) {
      return status;
    }
    uint32_t type;
    InodeID child_ino;
    status = ParseDirValue(entry_value, &type, &child_ino);
    if (!status.ok()) {
      return status;
    }
    status = txn.Get(key_.Inode(child_ino), &value);
    if (!status.ok()) {
      return status;
    }
    status = ParseInode(value, &child);
    if (!status.ok()) {
      return status;
    }
    if (!child.IsDir()) {
      return Status::NotDirectory("not a directory");
    }
    if (!parent.CheckStickyDelete(ctx.uid, child)) {
      return Status::Permission("sticky bit denied");
    }
    uint64_t length;
    status = txn.HLen(key_.Directory(child_ino), &length);
    if (!status.ok()) {
      return status;
    }
    if (length != 0) {
      return Status::NotEmpty("directory not empty");
    }
    parent.attr.nlink--;
    Touch(&parent, false, true, true);
    status = txn.HDel(key_.Directory(parent_ino), name);
    if (!status.ok()) {
      return status;
    }
    status = txn.Del(key_.Directory(child_ino));
    if (!status.ok()) {
      return status;
    }
    status = txn.Del(key_.Inode(child_ino));
    if (!status.ok()) {
      return status;
    }
    status = SerializeInode(parent, &value);
    if (!status.ok()) {
      return status;
    }
    status = txn.Set(key_.Inode(parent_ino), value);
    if (!status.ok()) {
      return status;
    }
    return txn.IncrBy(key_.InodeCount(), -1);
  });
}

Status RedisMetaImpl::Rename(InodeID old_parent_ino, std::string_view old_name, InodeID new_parent_ino,
                             std::string_view new_name, RenameFlag flags, RenameResult *result) {
  if (result) {
    *result = {};
  }
  if (old_name.size() > 255 || new_name.size() > 255) {
    return Status::NameTooLong("file name exceeds maximum length");
  }
  if (old_name == "." || old_name == ".." || new_name == "." || new_name == "..") {
    return Status::Busy("cannot rename . or ..");
  }
  const auto ctx = folly::fibers::local<SwordFsContext>();
  return client_->Transact([&](RedisMetaTxn &txn) {
    std::string old_parent_value, new_parent_value, source_value, source_entry_value, target_entry_value;
    auto status = txn.Get(key_.Inode(old_parent_ino), &old_parent_value);
    if (!status.ok()) {
      return status;
    }
    SwordFsInode old_parent;
    status = ParseInode(old_parent_value, &old_parent);
    if (!status.ok()) {
      return status;
    }
    if (!old_parent.IsDir()) {
      return Status::NotDirectory("old parent is not a directory");
    }
    status = txn.Get(key_.Inode(new_parent_ino), &new_parent_value);
    if (!status.ok()) {
      return status;
    }
    SwordFsInode new_parent;
    status = ParseInode(new_parent_value, &new_parent);
    if (!status.ok()) {
      return status;
    }
    if (!new_parent.IsDir()) {
      return Status::NotDirectory("new parent is not a directory");
    }
    if (!old_parent.CheckAccess(ctx.uid, ctx.gid, W_OK | X_OK)) {
      return Status::Permission("access denied on old parent");
    }
    if (!new_parent.CheckAccess(ctx.uid, ctx.gid, W_OK | X_OK)) {
      return Status::Permission("access denied on new parent");
    }
    status = txn.HGet(key_.Directory(old_parent_ino), old_name, &source_entry_value);
    if (!status.ok()) {
      return status;
    }
    uint32_t source_type;
    InodeID source_ino;
    status = ParseDirValue(source_entry_value, &source_type, &source_ino);
    if (!status.ok()) {
      return status;
    }
    status = txn.Get(key_.Inode(source_ino), &source_value);
    if (!status.ok()) {
      return status;
    }
    SwordFsInode source;
    status = ParseInode(source_value, &source);
    if (!status.ok()) {
      return status;
    }
    if (!old_parent.CheckStickyDelete(ctx.uid, source)) {
      return Status::Permission("sticky bit denied on source");
    }

    auto is_descendant = [&](InodeID ancestor_ino, InodeID candidate_ino, bool *result_out) -> Status {
      if (result_out == nullptr) {
        return Status::InvalidArgument("cycle check output is null");
      }
      *result_out = false;
      InodeID current_ino = candidate_ino;
      std::unordered_set<InodeID> visited;
      while (current_ino != 0) {
        if (!visited.insert(current_ino).second) {
          return Status::Malformed("directory parent cycle detected");
        }
        if (current_ino == ancestor_ino) {
          *result_out = true;
          return Status::OK();
        }
        std::string inode_value;
        auto status = txn.Get(key_.Inode(current_ino), &inode_value);
        if (!status.ok()) {
          return status;
        }
        SwordFsInode inode;
        status = ParseInode(inode_value, &inode);
        if (!status.ok()) {
          return status;
        }
        if (inode.parent_ino == current_ino) {
          break;
        }
        current_ino = inode.parent_ino;
      }
      return Status::OK();
    };

    if (source.IsDir()) {
      bool cycle = false;
      status = is_descendant(source_ino, new_parent_ino, &cycle);
      if (!status.ok()) {
        return status;
      }
      if (cycle) {
        return Status::InvalidArgument("cannot move directory into its descendant");
      }
    }

    status = txn.HGet(key_.Directory(new_parent_ino), new_name, &target_entry_value);
    const bool target_exists = status.ok();
    if (!target_exists && !status.IsNotFound()) {
      return status;
    }
    if (HasRenameFlag(flags, RenameFlag::kNoReplace) && target_exists) {
      return Status::AlreadyExists("target entry exists");
    }
    if (HasRenameFlag(flags, RenameFlag::kExchange)) {
      if (!target_exists) {
        return Status::NotFound("target does not exist for RENAME_EXCHANGE");
      }
      uint32_t target_type;
      InodeID target_ino;
      status = ParseDirValue(target_entry_value, &target_type, &target_ino);
      if (!status.ok()) {
        return status;
      }
      if (target_ino == source_ino) {
        return Status::OK();
      }
      SwordFsInode target;
      status = txn.Get(key_.Inode(target_ino), &source_value);
      if (!status.ok()) {
        return status;
      }
      status = ParseInode(source_value, &target);
      if (!status.ok()) {
        return status;
      }
      if (!new_parent.CheckStickyDelete(ctx.uid, target)) {
        return Status::Permission("sticky bit denied on target");
      }
      if (source.IsDir() != target.IsDir()) {
        return Status::InvalidArgument("cannot exchange directory with non-directory");
      }
      if (target.IsDir()) {
        bool cycle = false;
        status = is_descendant(target_ino, old_parent_ino, &cycle);
        if (!status.ok()) {
          return status;
        }
        if (cycle) {
          return Status::InvalidArgument("cannot exchange directory into its descendant");
        }
      }
      status = txn.HSet(key_.Directory(old_parent_ino), old_name, SerializeDirValue(target_type, target_ino));
      if (!status.ok()) {
        return status;
      }
      status = txn.HSet(key_.Directory(new_parent_ino), new_name, SerializeDirValue(source_type, source_ino));
      if (!status.ok()) {
        return status;
      }
      source.parent_ino = new_parent_ino;
      target.parent_ino = old_parent_ino;
      Touch(&source, false, false, true);
      Touch(&target, false, false, true);
      Touch(&old_parent, false, true, true);
      Touch(&new_parent, false, true, true);
      status = SerializeInode(source, &source_value);
      if (!status.ok()) {
        return status;
      }
      status = txn.Set(key_.Inode(source_ino), source_value);
      if (!status.ok()) {
        return status;
      }
      status = SerializeInode(target, &source_value);
      if (!status.ok()) {
        return status;
      }
      status = txn.Set(key_.Inode(target_ino), source_value);
      if (!status.ok()) {
        return status;
      }
      status = SerializeInode(old_parent, &old_parent_value);
      if (!status.ok()) {
        return status;
      }
      status = txn.Set(key_.Inode(old_parent_ino), old_parent_value);
      if (!status.ok()) {
        return status;
      }
      if (new_parent_ino != old_parent_ino) {
        status = SerializeInode(new_parent, &new_parent_value);
        if (!status.ok()) {
          return status;
        }
        status = txn.Set(key_.Inode(new_parent_ino), new_parent_value);
      }
      return status;
    }
    if (target_exists) {
      uint32_t target_type;
      InodeID target_ino;
      status = ParseDirValue(target_entry_value, &target_type, &target_ino);
      if (!status.ok()) {
        return status;
      }
      if (target_ino == source_ino) {
        return Status::OK();
      }
      SwordFsInode target;
      status = txn.Get(key_.Inode(target_ino), &source_value);
      if (!status.ok()) {
        return status;
      }
      status = ParseInode(source_value, &target);
      if (!status.ok()) {
        return status;
      }
      if (!new_parent.CheckStickyDelete(ctx.uid, target)) {
        return Status::Permission("sticky bit denied on target");
      }
      if (source.IsDir() != target.IsDir()) {
        return source.IsDir() ? Status::NotDirectory("target is not a directory")
                              : Status::IsDirectory("target is a directory");
      }
      if (target.IsDir()) {
        uint64_t length;
        status = txn.HLen(key_.Directory(target_ino), &length);
        if (!status.ok()) {
          return status;
        }
        if (length != 0) {
          return Status::NotEmpty("target directory not empty");
        }
        status = txn.Del(key_.Directory(target_ino));
        if (!status.ok()) {
          return status;
        }
        status = txn.Del(key_.Inode(target_ino));
        if (!status.ok()) {
          return status;
        }
        status = txn.IncrBy(key_.InodeCount(), -1);
        if (!status.ok()) {
          return status;
        }
        // Removing the victim directory removes its ".." backlink from its
        // parent. Keep the copy that will be serialized below authoritative
        // when both parent inodes are the same.
        if (old_parent_ino == new_parent_ino) {
          old_parent.attr.nlink--;
        } else {
          new_parent.attr.nlink--;
        }
      } else {
        if (target.attr.nlink > 0) {
          target.attr.nlink--;
        }
        Touch(&target, false, false, true);
        if (target.attr.nlink == 0) { /* retain orphan for VFS reclaim */
        }
        status = SerializeInode(target, &source_value);
        if (!status.ok()) {
          return status;
        }
        status = txn.Set(key_.Inode(target_ino), source_value);
        if (!status.ok()) {
          return status;
        }
        if (result) {
          result->overwritten_ino = target_ino;
          result->overwritten_post_nlink = target.attr.nlink;
        }
      }
    }
    status = txn.HDel(key_.Directory(old_parent_ino), old_name);
    if (!status.ok()) {
      return status;
    }
    status = txn.HSet(key_.Directory(new_parent_ino), new_name, SerializeDirValue(source_type, source_ino));
    if (!status.ok()) {
      return status;
    }
    source.parent_ino = new_parent_ino;
    // Moving a directory re-parents its "..": the old parent loses a hard
    // link and the new parent gains one. This applies whether or not the
    // move replaced a victim; the victim's own ".." backlink was already
    // accounted for above.
    if (source.IsDir() && old_parent_ino != new_parent_ino) {
      old_parent.attr.nlink--;
      new_parent.attr.nlink++;
    }
    Touch(&source, false, false, true);
    Touch(&old_parent, false, true, true);
    Touch(&new_parent, false, true, true);
    status = SerializeInode(source, &source_value);
    if (!status.ok()) {
      return status;
    }
    status = txn.Set(key_.Inode(source_ino), source_value);
    if (!status.ok()) {
      return status;
    }
    status = SerializeInode(old_parent, &old_parent_value);
    if (!status.ok()) {
      return status;
    }
    status = txn.Set(key_.Inode(old_parent_ino), old_parent_value);
    if (!status.ok()) {
      return status;
    }
    if (new_parent_ino != old_parent_ino) {
      status = SerializeInode(new_parent, &new_parent_value);
      if (!status.ok()) {
        return status;
      }
      status = txn.Set(key_.Inode(new_parent_ino), new_parent_value);
    }
    return status;
  });
}

Status RedisMetaImpl::SetAttr(InodeID ino, const SwordFsAttr &requested, SetAttrField fields, SwordFsInode *out) {
  return client_->Transact([&](RedisMetaTxn &txn) {
    std::string value;
    auto status = txn.Get(key_.Inode(ino), &value);
    if (!status.ok()) {
      return status;
    }
    SwordFsInode inode;
    status = ParseInode(value, &inode);
    if (!status.ok()) {
      return status;
    }
    const bool size_changed = HasSetAttrField(fields, SetAttrField::kSize) && inode.attr.size != requested.size;
    const bool mtime_changed = size_changed && !HasSetAttrField(fields, SetAttrField::kMtime) &&
                               !HasSetAttrField(fields, SetAttrField::kMtimeNow);
    if (HasSetAttrField(fields, SetAttrField::kMode)) {
      inode.attr.mode = (inode.attr.mode & S_IFMT) | (requested.mode & 07777);
    }
    if (HasSetAttrField(fields, SetAttrField::kUid)) {
      inode.attr.uid = requested.uid;
    }
    if (HasSetAttrField(fields, SetAttrField::kGid)) {
      inode.attr.gid = requested.gid;
    }
    if (HasSetAttrField(fields, SetAttrField::kSize)) {
      inode.attr.size = requested.size;
      if (mtime_changed) {
        Touch(&inode, false, true, false);
      }
    }
    if (HasSetAttrField(fields, SetAttrField::kAtime)) {
      inode.attr.atime = requested.atime;
      inode.attr.atime_nsec = requested.atime_nsec;
    }
    if (HasSetAttrField(fields, SetAttrField::kMtime)) {
      inode.attr.mtime = requested.mtime;
      inode.attr.mtime_nsec = requested.mtime_nsec;
    }
    if (HasSetAttrField(fields, SetAttrField::kAtimeNow)) {
      Touch(&inode, true, false, false);
    }
    if (HasSetAttrField(fields, SetAttrField::kMtimeNow)) {
      Touch(&inode, false, true, false);
    }
    if (size_changed || HasSetAttrField(fields, SetAttrField::kUid) || HasSetAttrField(fields, SetAttrField::kGid)) {
      inode.attr.KillSUID();
    }
    Touch(&inode, false, false, true);
    if (size_changed) {
      std::vector<std::pair<std::string, std::string>> chunks;
      status = txn.HGetAll(key_.Chunk(ino), &chunks);
      if (!status.ok()) {
        return status;
      }
      for (const auto &[field, chunk_value] : chunks) {
        SwordFsChunk chunk;
        status = chunk.ParseFrom(chunk_value);
        if (!status.ok()) {
          return status;
        }
        if (chunk.start_offset >= inode.attr.size) {
          status = txn.HDel(key_.Chunk(ino), field);
          if (!status.ok()) {
            return status;
          }
        } else {
          const uint64_t new_chunk_size = inode.attr.size - chunk.start_offset;
          if (chunk.size > new_chunk_size) {
            chunk.size = new_chunk_size;
            status = chunk.SerializeTo(&value);
            if (!status.ok()) {
              return status;
            }
            status = txn.HSet(key_.Chunk(ino), field, value);
            if (!status.ok()) {
              return status;
            }
          }
        }
      }
    }
    status = SerializeInode(inode, &value);
    if (!status.ok()) {
      return status;
    }
    status = txn.Set(key_.Inode(ino), value);
    if (!status.ok()) {
      return status;
    }
    if (out) {
      *out = inode;
    }
    return Status::OK();
  });
}

Status RedisMetaImpl::StatFs(SwordFsStatFs *stbuf) {
  if (!stbuf) {
    return Status::InvalidArgument("statfs output is null");
  }
  std::string value;
  auto status = client_->Get(key_.InodeCount(), &value);
  if (!status.ok()) {
    return status;
  }
  uint64_t files = 0;
  const auto [end, error] = std::from_chars(value.data(), value.data() + value.size(), files);
  if (error != std::errc{} || end != value.data() + value.size()) {
    return Status::IOError("invalid Redis inode count");
  }
  *stbuf = {};
  stbuf->name_max = 255;
  stbuf->fragment_size = 4096;
  stbuf->block_size = 4096;
  stbuf->blocks = 268435456;
  stbuf->blocks_free = stbuf->blocks_available = stbuf->blocks;
  stbuf->files = files;
  stbuf->files_free = UINT64_MAX;
  return Status::OK();
}

Status RedisMetaImpl::Access(InodeID ino, uint32_t mask) {
  const auto ctx = folly::fibers::local<SwordFsContext>();
  return client_->Transact([&](RedisMetaTxn &txn) {
    std::string value;
    auto status = txn.Get(key_.Inode(ino), &value);
    if (!status.ok()) {
      return status;
    }
    SwordFsInode inode;
    status = ParseInode(value, &inode);
    if (!status.ok()) {
      return status;
    }
    return inode.CheckAccess(ctx.uid, ctx.gid, mask) ? Status::OK() : Status::Permission("access denied");
  });
}

Status RedisMetaImpl::Symlink(InodeID parent_ino, std::string_view name, std::string_view link, SwordFsInode *out) {
  if (name.size() > 255) {
    return Status::NameTooLong("symlink name exceeds maximum length");
  }
  const auto ctx = folly::fibers::local<SwordFsContext>();
  InodeID child_ino;
  auto status = client_->Incr(key_.NextIno(), &child_ino);
  if (!status.ok()) {
    return status;
  }
  SwordFsInode child;
  status = client_->Transact([&](RedisMetaTxn &txn) {
    std::string value, ignored;
    auto status = txn.Get(key_.Inode(parent_ino), &value);
    if (!status.ok()) {
      return status;
    }
    SwordFsInode parent;
    status = ParseInode(value, &parent);
    if (!status.ok()) {
      return status;
    }
    if (!parent.IsDir()) {
      return Status::NotDirectory("parent is not a directory");
    }
    if (!parent.CheckAccess(ctx.uid, ctx.gid, W_OK | X_OK)) {
      return Status::Permission("access denied on parent");
    }
    status = txn.HGet(key_.Directory(parent_ino), name, &ignored);
    if (status.ok()) {
      return Status::AlreadyExists("entry already exists");
    }
    if (!status.IsNotFound()) {
      return status;
    }
    SwordFsAttr attr(child_ino, S_IFLNK | 0777u);
    attr.uid = ctx.uid;
    attr.gid = parent.attr.gid;
    attr.size = link.size();
    child = SwordFsInode(child_ino, attr, parent_ino, std::string(link));
    Touch(&parent, false, true, true);
    status = SerializeInode(child, &value);
    if (!status.ok()) {
      return status;
    }
    status = txn.Set(key_.Inode(child_ino), value);
    if (!status.ok()) {
      return status;
    }
    status = txn.HSet(key_.Directory(parent_ino), name, SerializeDirValue(DT_LNK, child_ino));
    if (!status.ok()) {
      return status;
    }
    status = SerializeInode(parent, &value);
    if (!status.ok()) {
      return status;
    }
    status = txn.Set(key_.Inode(parent_ino), value);
    if (!status.ok()) {
      return status;
    }
    return txn.IncrBy(key_.InodeCount(), 1);
  });
  if (status.ok() && out) {
    *out = child;
  }
  return status;
}

Status RedisMetaImpl::Link(InodeID ino, InodeID newparent_ino, std::string_view newname, SwordFsInode *out) {
  if (newname.size() > 255) {
    return Status::NameTooLong("link name exceeds maximum length");
  }
  const auto ctx = folly::fibers::local<SwordFsContext>();
  return client_->Transact([&](RedisMetaTxn &txn) {
    std::string value, ignored;
    auto status = txn.Get(key_.Inode(ino), &value);
    if (!status.ok()) {
      return status;
    }
    SwordFsInode inode;
    status = ParseInode(value, &inode);
    if (!status.ok()) {
      return status;
    }
    if (inode.IsDir()) {
      return Status::NotPermitted("cannot hard-link directory");
    }
    status = txn.Get(key_.Inode(newparent_ino), &value);
    if (!status.ok()) {
      return status;
    }
    SwordFsInode parent;
    status = ParseInode(value, &parent);
    if (!status.ok()) {
      return status;
    }
    if (!parent.IsDir()) {
      return Status::NotDirectory("new parent is not a directory");
    }
    if (!parent.CheckAccess(ctx.uid, ctx.gid, W_OK | X_OK)) {
      return Status::Permission("access denied on parent");
    }
    status = txn.HGet(key_.Directory(newparent_ino), newname, &ignored);
    if (status.ok()) {
      return Status::AlreadyExists("entry already exists");
    }
    if (!status.IsNotFound()) {
      return status;
    }
    inode.attr.nlink++;
    Touch(&inode, false, false, true);
    Touch(&parent, false, true, true);
    status = txn.HSet(key_.Directory(newparent_ino), newname, SerializeDirValue(ModeToDt(inode.attr.mode), ino));
    if (!status.ok()) {
      return status;
    }
    status = SerializeInode(inode, &value);
    if (!status.ok()) {
      return status;
    }
    status = txn.Set(key_.Inode(ino), value);
    if (!status.ok()) {
      return status;
    }
    status = SerializeInode(parent, &value);
    if (!status.ok()) {
      return status;
    }
    status = txn.Set(key_.Inode(newparent_ino), value);
    if (status.ok() && out) {
      *out = inode;
    }
    return status;
  });
}

Status RedisMetaImpl::Readlink(InodeID ino, std::string *target) {
  if (!target) {
    return Status::InvalidArgument("Readlink output is null");
  }
  return client_->Transact([&](RedisMetaTxn &txn) {
    std::string value;
    auto status = txn.Get(key_.Inode(ino), &value);
    if (!status.ok()) {
      return status;
    }
    SwordFsInode inode;
    status = ParseInode(value, &inode);
    if (!status.ok()) {
      return status;
    }
    if (!inode.IsSymlink()) {
      return Status::InvalidArgument("not a symbolic link");
    }
    *target = inode.symlink_target;
    return Status::OK();
  });
}

Status RedisMetaImpl::Open(InodeID ino) {
  return client_->Transact([&](RedisMetaTxn &txn) {
    std::string value;
    auto status = txn.Get(key_.Inode(ino), &value);
    if (!status.ok()) {
      return status;
    }
    SwordFsInode inode;
    status = ParseInode(value, &inode);
    if (!status.ok()) {
      return status;
    }
    if (!inode.IsRegular()) {
      return Status::NotDirectory("not a regular file");
    }
    const auto ctx = folly::fibers::local<SwordFsContext>();
    if (!inode.CheckAccess(ctx.uid, ctx.gid, R_OK)) {
      return Status::Permission("access denied");
    }
    Touch(&inode, true, false, false);
    status = SerializeInode(inode, &value);
    if (!status.ok()) {
      return status;
    }
    return txn.Set(key_.Inode(ino), value);
  });
}

Status RedisMetaImpl::ReclaimInode(InodeID ino) {
  return client_->Transact([&](RedisMetaTxn &txn) {
    std::string value;
    auto status = txn.Get(key_.Inode(ino), &value);
    if (status.IsNotFound()) {
      return Status::OK();
    }
    if (!status.ok()) {
      return status;
    }
    SwordFsInode inode;
    status = ParseInode(value, &inode);
    if (!status.ok()) {
      return status;
    }
    if (inode.attr.nlink != 0) {
      return Status::OK();
    }
    status = txn.Del(key_.Chunk(ino));
    if (!status.ok()) {
      return status;
    }
    status = txn.Del(key_.Inode(ino));
    if (!status.ok()) {
      return status;
    }
    return txn.IncrBy(key_.InodeCount(), -1);
  });
}

Status RedisMetaImpl::ListChunks(InodeID ino, std::vector<SwordFsChunk> *out) {
  if (!out) {
    return Status::InvalidArgument("ListChunks output is null");
  }
  return client_->Transact([&](RedisMetaTxn &txn) {
    std::string value;
    auto status = txn.Get(key_.Inode(ino), &value);
    if (!status.ok()) {
      return status;
    }
    std::vector<std::pair<std::string, std::string>> values;
    status = txn.HGetAll(key_.Chunk(ino), &values);
    if (!status.ok()) {
      return status;
    }
    out->clear();
    for (const auto &[field, chunk_value] : values) {
      SwordFsChunk chunk;
      status = chunk.ParseFrom(chunk_value);
      if (!status.ok()) {
        return status;
      }
      out->push_back(std::move(chunk));
    }
    std::sort(out->begin(), out->end(), [](const auto &a, const auto &b) { return a.index < b.index; });
    return Status::OK();
  });
}

Status RedisMetaImpl::OpenDir(InodeID ino) {
  return client_->Transact([&](RedisMetaTxn &txn) {
    std::string value;
    auto status = txn.Get(key_.Inode(ino), &value);
    if (!status.ok()) {
      return status;
    }
    SwordFsInode dir;
    status = ParseInode(value, &dir);
    if (!status.ok()) {
      return status;
    }
    if (!dir.IsDir()) {
      return Status::NotDirectory("not a directory");
    }
    Touch(&dir, true, false, false);
    status = SerializeInode(dir, &value);
    if (!status.ok()) {
      return status;
    }
    return txn.Set(key_.Inode(ino), value);
  });
}

Status RedisMetaImpl::AddChunk(InodeID ino, const SwordFsChunk &chunk) {
  std::string chunk_value;
  auto status = chunk.SerializeTo(&chunk_value);
  if (!status.ok()) {
    return status;
  }
  return client_->Transact([&](RedisMetaTxn &txn) {
    std::string value;
    auto status = txn.Get(key_.Inode(ino), &value);
    if (!status.ok()) {
      return status;
    }
    SwordFsInode inode;
    status = ParseInode(value, &inode);
    if (!status.ok()) {
      return status;
    }
    if (!inode.IsRegular()) {
      return Status::InvalidArgument("not a regular file");
    }
    return txn.HSet(key_.Chunk(ino), std::to_string(chunk.index), chunk_value);
  });
}

Status RedisMetaImpl::FindChunk(InodeID ino, ChunkIndex idx, SwordFsChunk *chunk) {
  if (!chunk) {
    return Status::InvalidArgument("FindChunk output is null");
  }
  return client_->Transact([&](RedisMetaTxn &txn) {
    std::string value;
    auto status = txn.HGet(key_.Chunk(ino), std::to_string(idx), &value);
    if (!status.ok()) {
      return status;
    }
    return chunk->ParseFrom(value);
  });
}

Status RedisMetaImpl::Truncate(InodeID ino, uint64_t size) {
  return client_->Transact([&](RedisMetaTxn &txn) {
    std::string value;
    auto status = txn.Get(key_.Inode(ino), &value);
    if (!status.ok()) {
      return status;
    }
    SwordFsInode inode;
    status = ParseInode(value, &inode);
    if (!status.ok()) {
      return status;
    }
    if (inode.attr.size == size) {
      return Status::OK();
    }
    inode.attr.size = size;
    inode.attr.KillSUID();
    Touch(&inode, false, true, true);
    std::vector<std::pair<std::string, std::string>> chunks;
    status = txn.HGetAll(key_.Chunk(ino), &chunks);
    if (!status.ok()) {
      return status;
    }
    for (const auto &[field, chunk_value] : chunks) {
      SwordFsChunk chunk;
      status = chunk.ParseFrom(chunk_value);
      if (!status.ok()) {
        return status;
      }
      if (chunk.start_offset >= size) {
        status = txn.HDel(key_.Chunk(ino), field);
        if (!status.ok()) {
          return status;
        }
      } else {
        const uint64_t new_chunk_size = size - chunk.start_offset;
        if (chunk.size > new_chunk_size) {
          chunk.size = new_chunk_size;
          status = chunk.SerializeTo(&value);
          if (!status.ok()) {
            return status;
          }
          status = txn.HSet(key_.Chunk(ino), field, value);
          if (!status.ok()) {
            return status;
          }
        }
      }
    }
    status = SerializeInode(inode, &value);
    if (!status.ok()) {
      return status;
    }
    return txn.Set(key_.Inode(ino), value);
  });
}

}  // namespace swordfs::metadata
