// Copyright 2026 SwordFS Contributors.
// Licensed under the Apache License, Version 2.0.

#include "metadata/mem/MemMetaImpl.hpp"

#include <dirent.h>
#include <folly/fibers/FiberManagerInternal.h>
#include <folly/logging/xlog.h>
#include <sys/stat.h>
#include <sys/types.h>

#include <algorithm>
#include <cerrno>
#include <cstring>
#include <utility>

#include "metadata/MetaEngineRegistry.hpp"
#include "metadata/Utils.hpp"
#include "metadata/mem/MemDirIterator.hpp"
#include "metadata/mem/VolumeFile.hpp"
#include "metadata/types/Common.hpp"
#include "metadata/types/Entry.hpp"
#include "metadata/types/Inode.hpp"
#include "metadata/types/Volume.hpp"
#include "utils/ExecutionDomain.hpp"
#include "utils/Logging.hpp"

namespace swordfs::metadata {

constexpr Limits kMemLimits{.max_name_length = kMaxNameLength, .max_free_inodes = UINT64_MAX};

const RegisterMetaEngine kMemoryMetaEngine{"memory", MemMetaImpl::CreateInstance};

MemMetaImpl::MemMetaImpl() {
  utils::ExpectInThreadDomain();
}

utils::Status MemMetaImpl::CreateInstance(std::string_view, std::string_view, std::unique_ptr<IMetaEngine> *out) {
  utils::ExpectInThreadDomain();
  if (out == nullptr) {
    return utils::Status::InvalidArgument("metadata engine output is null");
  }
  *out = std::make_unique<MemMetaImpl>();
  return utils::Status::OK();
}

MemMetaImpl::~MemMetaImpl() {
  utils::ExpectInThreadDomain();
}

// Transaction model: every method below runs its metadata mutation as a
// single store_.Transact() script, so each IMetaEngine operation is atomic.
//
// Discipline: a Transact() script contains MemMetaTxn semantic operations
// plus pure policy decisions on value snapshots. Logging and ambient
// context are captured outside the transaction. MemMetaTxn owns metadata
// state transitions that must remain atomic; this layer should not replay
// their bookkeeping by hand.
//
// Division of labour: the primitives maintain the tree's structural
// invariants (parent nlink on directory link/unlink/move, mtime/ctime
// on entry-list changes, ctime on re-linked inodes) — this layer only
// contributes POLICY: name validation, type/namespace validation, cycle
// prevention, rename flags, and POSIX error codes. Caller authorization is
// owned by Linux VFS/FUSE `default_permissions`, not reconstructed here.
//
// Lookup idiom: check the Lookup status first and return it unchanged
// (a missing inode is NotFound, NOT NotDirectory), then test the type
// predicate on the snapshot:
//
//   SwordFsInode parent;
//   Status status = txn.LookupInode(parent_ino, &parent);
//   if (!status.ok()) {
//     return status;
//   }
//   if (!parent.IsDir()) {
//     return Status::NotDirectory("parent is not a directory");
//   }

// ────────────────────────────────────────────────────────────────
// Volume operations
// ────────────────────────────────────────────────────────────────

Status MemMetaImpl::Initialize() {
  utils::ExpectInThreadDomain();
  return Status::OK();
}

Status MemMetaImpl::FormatVolume(const SwordFsVolume &config) {
  utils::ExpectInThreadDomain();
  mem::VolumeFile file{config.name};
  if (file.Exists()) {
    return Status::AlreadyExists("volume already exists: " + config.name);
  }
  auto status = file.Write(config);
  if (status.ok()) {
    chunk_size_ = config.chunk_size;
  }
  return status;
}

Status MemMetaImpl::LoadVolume(SwordFsVolume *config) {
  utils::ExpectInThreadDomain();
  if (config == nullptr) {
    return Status::InvalidArgument("memory volume config output is null");
  }
  auto status = mem::VolumeFile(config->name).Read(config);
  if (status.ok()) {
    chunk_size_ = config->chunk_size;
  }
  return status;
}

// ────────────────────────────────────────────────────────────────
// Entry operations
// ────────────────────────────────────────────────────────────────

Status MemMetaImpl::Lookup(InodeID parent_ino, std::string_view name, SwordFsInode *out) {
  utils::ExpectInFiberDomain();
  if (auto status = ValidateNameComponent(name); !status.ok()) {
    return status;
  }
  SwordFsInode child;
  Status status = store_.Transact([&](MemMetaTxn &txn) -> Status {
    Status status = txn.LookupEntry(parent_ino, name, &child);
    if (!status.ok()) {
      return status;
    }
    return Status::OK();
  });

  if (!status.ok()) {
    SWORDFS_LOG_DEBUG << "Lookup: parent=" << parent_ino << " name='" << name << "' failed: " << status.message();
    return status;
  }
  SWORDFS_LOG_DEBUG << "Lookup: parent=" << parent_ino << " name='" << name << "' -> ino=" << child.ino;

  if (out) {
    *out = child;
  }
  return Status::OK();
}

Status MemMetaImpl::GetInode(InodeID ino, SwordFsInode *out) {
  utils::ExpectInFiberDomain();
  SwordFsInode inode;
  Status status = store_.Transact([&](MemMetaTxn &txn) -> Status { return txn.LookupInode(ino, &inode); });

  if (!status.ok()) {
    SWORDFS_LOG_DEBUG << "GetInode: ino " << ino << " failed: " << status.message();
    return status;
  }
  if (out) {
    *out = inode;
  }
  return Status::OK();
}

Status MemMetaImpl::GetInodes(const std::vector<InodeID> &inode_ids, std::vector<std::optional<SwordFsInode>> *out) {
  utils::ExpectInFiberDomain();
  if (out == nullptr) {
    return Status::InvalidArgument("inode batch output is null");
  }

  std::vector<std::optional<SwordFsInode>> result;
  result.reserve(inode_ids.size());
  store_.Transact([&](MemMetaTxn &txn) {
    for (const InodeID requested_ino : inode_ids) {
      SwordFsInode inode;
      // MemMetaTxn::LookupInode has exactly two outcomes: present or missing.
      // Unlike remote backends there is no transport/decoding failure to
      // propagate, so keep the Memory batch path free of unreachable status
      // plumbing.
      if (!txn.LookupInode(requested_ino, &inode).ok()) {
        result.emplace_back(std::nullopt);
        continue;
      }
      result.emplace_back(std::move(inode));
    }
  });
  *out = std::move(result);
  return Status::OK();
}

Status MemMetaImpl::Create(InodeID parent_ino, std::string_view name, uint32_t mode, SwordFsInode *out) {
  utils::ExpectInFiberDomain();
  if (auto status = ValidateNameComponent(name); !status.ok()) {
    return status;
  }

  SwordFsInode child;
  Status status = store_.Transact([&](MemMetaTxn &txn) -> Status {
    SwordFsInode parent;
    Status status = txn.LookupInode(parent_ino, &parent);
    if (!status.ok()) {
      return status;
    }
    if (!parent.IsDir()) {
      return Status::NotDirectory("parent is not a directory");
    }
    uint32_t file_mode = static_cast<uint32_t>(S_IFREG) | (mode & 0777u);
    return txn.AddEntry(parent_ino, name, file_mode, &child);
  });

  if (!status.ok()) {
    SWORDFS_LOG_DEBUG << "Create: parent=" << parent_ino << " name='" << name << "' failed: " << status.message();
    return status;
  }

  SWORDFS_LOG_DEBUG << "Create: parent=" << parent_ino << " name='" << name << "' -> ino=" << child.ino;
  if (out) {
    *out = child;
  }
  return Status::OK();
}

Status MemMetaImpl::MkNod(InodeID parent_ino, std::string_view name, uint32_t mode, uint64_t rdev, SwordFsInode *out) {
  utils::ExpectInFiberDomain();
  if (auto status = ValidateNameComponent(name); !status.ok()) {
    return status;
  }
  if (auto status = ValidateMknodMode(mode); !status.ok()) {
    return status;
  }

  SwordFsInode child;
  const uint64_t normalized_rdev = NormalizeMknodRdev(mode, rdev);
  Status status = store_.Transact([&](MemMetaTxn &txn) -> Status {
    SwordFsInode parent;
    Status status = txn.LookupInode(parent_ino, &parent);
    if (!status.ok()) {
      return status;
    }
    if (!parent.IsDir()) {
      return Status::NotDirectory("parent is not a directory");
    }
    return txn.AddEntry(parent_ino, name, mode, normalized_rdev, &child);
  });

  if (!status.ok()) {
    SWORDFS_LOG_DEBUG << "MkNod: parent=" << parent_ino << " name='" << name << "' failed: " << status.message();
    return status;
  }
  if (out != nullptr) {
    *out = child;
  }
  return Status::OK();
}

Status MemMetaImpl::Unlink(InodeID parent_ino, std::string_view name) {
  utils::ExpectInFiberDomain();
  if (auto status = ValidateNameComponent(name); !status.ok()) {
    return status;
  }
  // Refuse to unlink "." or ".."
  if (name == "." || name == "..") {
    return Status::InvalidArgument("cannot unlink . or ..");
  }

  const SwordFsContext ctx = folly::fibers::local<SwordFsContext>();
  InodeID unlinked_ino = 0;
  Status status = store_.Transact([&](MemMetaTxn &txn) -> Status {
    SwordFsInode parent;
    Status status = txn.LookupInode(parent_ino, &parent);
    if (!status.ok()) {
      return status;
    }
    if (!parent.IsDir()) {
      return Status::NotDirectory("parent is not a directory");
    }

    SwordFsInode target;
    status = txn.LookupEntry(parent_ino, name, &target);
    if (!status.ok()) {
      return status;
    }

    if (!parent.CheckStickyDelete(ctx.uid, target)) {
      return Status::Permission("sticky bit denied");
    }

    if (target.IsDir()) {
      return Status::InvalidArgument("cannot unlink directory");
    }

    unlinked_ino = target.ino;
    return txn.Unlink(parent_ino, name);
  });

  if (!status.ok()) {
    SWORDFS_LOG_DEBUG << "Unlink: parent=" << parent_ino << " name='" << name << "' failed: " << status.message();
    return status;
  }
  SWORDFS_LOG_DEBUG << "Unlink: parent=" << parent_ino << " name='" << name << "' ino=" << unlinked_ino;
  return Status::OK();
}

Status MemMetaImpl::Rename(InodeID old_parent_ino, std::string_view old_name, InodeID new_parent_ino,
                           std::string_view new_name, RenameFlag flags) {
  utils::ExpectInFiberDomain();
  if (auto status = ValidateNameComponent(old_name); !status.ok()) {
    return status;
  }
  if (auto status = ValidateNameComponent(new_name); !status.ok()) {
    return status;
  }

  // "." and ".." cannot be renamed
  if (old_name == "." || old_name == ".." || new_name == "." || new_name == "..") {
    return Status::Busy("cannot rename . or ..");
  }

  const SwordFsContext ctx = folly::fibers::local<SwordFsContext>();

  // The whole rename — validation, target removal, and the move itself —
  // runs as one transaction so concurrent observers can never see an
  // intermediate state (e.g. target unlinked but source not yet moved).
  // All structural bookkeeping (nlink, parent_ino, timestamps) lives in
  // the txn primitives; what remains here is policy.
  Status status = store_.Transact([&](MemMetaTxn &txn) -> Status {
    // Both parents must exist and be directories.
    SwordFsInode old_parent;
    Status status = txn.LookupInode(old_parent_ino, &old_parent);
    if (!status.ok()) {
      return status;
    }
    if (!old_parent.IsDir()) {
      return Status::NotDirectory("old parent is not a directory");
    }
    SwordFsInode new_parent;
    status = txn.LookupInode(new_parent_ino, &new_parent);
    if (!status.ok()) {
      return status;
    }
    if (!new_parent.IsDir()) {
      return Status::NotDirectory("new parent is not a directory");
    }

    SwordFsInode moved;
    status = txn.LookupEntry(old_parent_ino, old_name, &moved);
    if (!status.ok()) {
      return Status::NotFound("source entry not found");
    }

    if (!old_parent.CheckStickyDelete(ctx.uid, moved)) {
      return Status::Permission("sticky bit denied on source");
    }

    SwordFsInode existing;
    bool target_exists = txn.LookupEntry(new_parent_ino, new_name, &existing).ok();

    if (target_exists && !new_parent.CheckStickyDelete(ctx.uid, existing)) {
      return Status::Permission("sticky bit denied on target");
    }

    if (HasRenameFlag(flags, RenameFlag::kExchange)) {
      // RENAME_EXCHANGE requires the target to exist, and POSIX forbids
      // exchanging a directory with a non-directory.  Cycle prevention
      // is enforced by SwapEntries itself.
      if (!target_exists) {
        return Status::NotFound("target does not exist for RENAME_EXCHANGE");
      }
      if (existing.IsDir() != moved.IsDir()) {
        return Status::InvalidArgument("cannot exchange directory with non-directory");
      }
      return txn.SwapEntries(old_parent_ino, old_name, new_parent_ino, new_name);
    }

    // Cycle prevention, the self-rename no-op, victim type checks and
    // victim removal are all enforced by MoveEntry; kNoReplace simply
    // withholds overwrite permission.
    return txn.MoveEntry(old_parent_ino, old_name, new_parent_ino, new_name,
                         !HasRenameFlag(flags, RenameFlag::kNoReplace));
  });

  if (!status.ok()) {
    SWORDFS_LOG_DEBUG << "Rename: " << old_parent_ino << "/'" << old_name << "' -> " << new_parent_ino << "/'"
                      << new_name << "' failed: " << status.message();
    return status;
  }
  SWORDFS_LOG_DEBUG << "Rename: " << old_parent_ino << "/'" << old_name << "' -> " << new_parent_ino << "/'" << new_name
                    << "'";
  return Status::OK();
}

Status MemMetaImpl::SetAttr(InodeID ino, const SwordFsAttr &attr, SetAttrField fields, SwordFsInode *out) {
  utils::ExpectInFiberDomain();
  SwordFsInode result;
  Status status = store_.Transact(
      [&](MemMetaTxn &txn) -> Status { return txn.SetAttr(ino, attr, fields, out ? &result : nullptr); });

  if (!status.ok()) {
    SWORDFS_LOG_DEBUG << "SetAttr: ino " << ino << " failed: " << status.message();
    return status;
  }
  if (out) {
    *out = result;
  }
  return Status::OK();
}

Status MemMetaImpl::Open(InodeID ino) {
  utils::ExpectInFiberDomain();
  Status status = store_.Transact([&](MemMetaTxn &txn) -> Status {
    SwordFsInode inode;
    Status status = txn.LookupInode(ino, &inode);
    if (!status.ok()) {
      return status;
    }

    // No `nlink == 0` check here. POSIX guarantees that an inode unlinked
    // while still open can be read/written through existing fds; the VFS
    // layer is also free to issue further Open() calls on the same inode
    // (re-open through /proc or other inode-by-number paths). Rejecting
    // `nlink == 0` here would break both cases. New opens by name go
    // through Lookup() at the VFS layer and never reach this code path
    // once the directory entry is gone.
    //
    // Only regular files can be opened (directories use OpenDir, symlinks
    // are resolved by the kernel).
    if (!inode.IsRegular()) {
      return Status::NotDirectory("not a regular file");
    }

    // Update atime on the file.
    return txn.TouchInode(ino, SetAttrField::kAtime);
  });

  if (!status.ok()) {
    SWORDFS_LOG_DEBUG << "Open: ino " << ino << " failed: " << status.message();
  }
  return status;
}

Status MemMetaImpl::PrepareReclaim(InodeID ino, std::optional<ReclaimWork> *work) {
  utils::ExpectInFiberDomain();
  if (work == nullptr) {
    return Status::InvalidArgument("reclaim work output is null");
  }
  return store_.Transact([&](MemMetaTxn &txn) { return txn.PrepareReclaim(ino, work); });
}

Status MemMetaImpl::CompleteReclaim(InodeID ino) {
  utils::ExpectInFiberDomain();
  return store_.Transact([&](MemMetaTxn &txn) { return txn.CompleteReclaim(ino); });
}

Status MemMetaImpl::VisitOrphanCandidates(const InodeVisitorFn &visitor) {
  utils::ExpectInFiberDomain();
  if (!visitor) {
    return Status::InvalidArgument("orphan candidate visitor is null");
  }

  std::vector<InodeID> candidates;
  store_.Transact([&](MemMetaTxn &txn) { return txn.ListOrphanCandidates(&candidates); });
  for (InodeID ino : candidates) {
    auto status = visitor(ino);
    if (!status.ok()) {
      return status;
    }
  }
  return Status::OK();
}

Status MemMetaImpl::VisitPendingReclaims(const ReclaimVisitorFn &visitor) {
  utils::ExpectInFiberDomain();
  if (!visitor) {
    return Status::InvalidArgument("pending reclaim visitor is null");
  }

  std::vector<ReclaimWork> pending;
  store_.Transact([&](MemMetaTxn &txn) { return txn.ListPendingReclaims(&pending); });
  for (const auto &work : pending) {
    auto status = visitor(work);
    if (!status.ok()) {
      return status;
    }
  }
  return Status::OK();
}

Status MemMetaImpl::VisitPendingDeletesBatch(size_t max_items, const PendingDeleteVisitorFn &visitor, bool *has_more) {
  utils::ExpectInFiberDomain();
  if (max_items == 0) {
    return Status::InvalidArgument("pending delete batch size must be positive");
  }
  if (!visitor) {
    return Status::InvalidArgument("pending delete visitor is null");
  }
  if (has_more == nullptr) {
    return Status::InvalidArgument("pending delete has-more output is null");
  }

  std::lock_guard<utils::FiberMutex> lock(pending_delete_scan_mutex_);
  *has_more = false;
  if (pending_delete_snapshot_offset_ >= pending_delete_snapshot_.size()) {
    pending_delete_snapshot_.clear();
    pending_delete_snapshot_offset_ = 0;
    auto status = store_.Transact([&](MemMetaTxn &txn) { return txn.ListPendingDeletes(pending_delete_snapshot_); });
    if (!status.ok()) {
      return status;
    }
  }

  size_t visited = 0;
  while (pending_delete_snapshot_offset_ < pending_delete_snapshot_.size() && visited < max_items) {
    const auto &work = pending_delete_snapshot_[pending_delete_snapshot_offset_];
    auto status = visitor(work);
    if (!status.ok()) {
      return status;
    }
    ++pending_delete_snapshot_offset_;
    ++visited;
  }

  *has_more = pending_delete_snapshot_offset_ < pending_delete_snapshot_.size();
  if (!*has_more) {
    pending_delete_snapshot_.clear();
    pending_delete_snapshot_offset_ = 0;
  }
  return Status::OK();
}

Status MemMetaImpl::CompletePendingDelete(std::string_view key) {
  utils::ExpectInFiberDomain();
  return store_.Transact([&](MemMetaTxn &txn) { return txn.CompletePendingDelete(key); });
}

Status MemMetaImpl::AllocateChunkRevision(ChunkRevision *revision) {
  return store_.Transact([&](MemMetaTxn &txn) { return txn.AllocateChunkRevision(revision); });
}

// ────────────────────────────────────────────────────────────────
// Directory operations
// ────────────────────────────────────────────────────────────────

Status MemMetaImpl::MkDir(InodeID parent_ino, std::string_view name, uint32_t mode, SwordFsInode *out) {
  utils::ExpectInFiberDomain();
  if (auto status = ValidateNameComponent(name); !status.ok()) {
    return status;
  }

  SwordFsInode child;
  Status status = store_.Transact([&](MemMetaTxn &txn) -> Status {
    SwordFsInode parent;
    Status status = txn.LookupInode(parent_ino, &parent);
    if (!status.ok()) {
      return status;
    }
    if (!parent.IsDir()) {
      return Status::NotDirectory("parent is not a directory");
    }

    uint32_t dir_mode = static_cast<uint32_t>(S_IFDIR) | (mode & 0777u);
    return txn.AddEntry(parent_ino, name, dir_mode, &child);
  });

  if (!status.ok()) {
    SWORDFS_LOG_DEBUG << "MkDir: parent=" << parent_ino << " name='" << name << "' failed: " << status.message();
    return status;
  }

  SWORDFS_LOG_DEBUG << "MkDir: parent=" << parent_ino << " name='" << name << "' -> ino=" << child.ino;

  if (out) {
    *out = child;
  }
  return Status::OK();
}

Status MemMetaImpl::RmDir(InodeID parent_ino, std::string_view name) {
  utils::ExpectInFiberDomain();
  if (auto status = ValidateNameComponent(name); !status.ok()) {
    return status;
  }
  // Cannot remove "." or ".."
  if (name == "." || name == "..") {
    return Status::InvalidArgument("cannot remove . or ..");
  }

  const SwordFsContext ctx = folly::fibers::local<SwordFsContext>();
  InodeID target_ino = 0;
  Status status = store_.Transact([&](MemMetaTxn &txn) -> Status {
    SwordFsInode parent;
    Status status = txn.LookupInode(parent_ino, &parent);
    if (!status.ok()) {
      return status;
    }
    if (!parent.IsDir()) {
      return Status::NotDirectory("parent is not a directory");
    }

    SwordFsInode target;
    status = txn.LookupEntry(parent_ino, name, &target);
    if (!status.ok()) {
      return status;
    }

    if (!parent.CheckStickyDelete(ctx.uid, target)) {
      return Status::Permission("sticky bit denied");
    }

    if (!target.IsDir()) {
      return Status::NotDirectory("not a directory");
    }

    target_ino = target.ino;

    // Unlink detaches the entry, drops the parent's nlink (the ".." backlink),
    // and reclaims the now-empty directory inode.
    return txn.Unlink(parent_ino, name);
  });

  if (!status.ok()) {
    SWORDFS_LOG_DEBUG << "RmDir: parent=" << parent_ino << " name='" << name << "' failed: " << status.message();
    return status;
  }
  SWORDFS_LOG_DEBUG << "RmDir: parent=" << parent_ino << " name='" << name << "' ino=" << target_ino;
  return Status::OK();
}

Status MemMetaImpl::OpenDir(InodeID ino, DirIteratorPtr *iterator) {
  utils::ExpectInFiberDomain();
  if (iterator == nullptr) {
    return Status::InvalidArgument("directory iterator output is null");
  }

  std::vector<SwordFsEntry> snapshot;
  auto status = store_.Transact([&](MemMetaTxn &txn) { return txn.ListEntries(ino, &snapshot); });
  if (!status.ok()) {
    SWORDFS_LOG_ERROR << "OpenDir: ino " << ino << " failed: " << status.message();
    return status;
  }

  *iterator = std::make_shared<MemDirIterator>(std::move(snapshot));

  // Directory atime is a best-effort side effect and must not make opening
  // the directory fail.
  status = store_.Transact([&](MemMetaTxn &txn) { return txn.TouchInode(ino, SetAttrField::kAtime); });
  if (!status.ok()) {
    SWORDFS_LOG_WARN << "OpenDir: failed to update atime for ino " << ino << ": " << status.message();
  }
  return Status::OK();
}

// ────────────────────────────────────────────────────────────────
// Link / symlink operations
// ────────────────────────────────────────────────────────────────

Status MemMetaImpl::Symlink(InodeID parent_ino, std::string_view name, std::string_view link, SwordFsInode *out) {
  utils::ExpectInFiberDomain();
  if (auto status = ValidateNameComponent(name); !status.ok()) {
    return status;
  }

  SwordFsInode child;
  Status status = store_.Transact([&](MemMetaTxn &txn) -> Status {
    SwordFsInode parent;
    Status status = txn.LookupInode(parent_ino, &parent);
    if (!status.ok()) {
      return status;
    }
    if (!parent.IsDir()) {
      return Status::NotDirectory("parent is not a directory");
    }

    uint32_t link_mode = static_cast<uint32_t>(S_IFLNK) | 0777u;
    status = txn.AddEntry(parent_ino, name, link_mode, &child);
    if (!status.ok()) {
      return status;
    }

    status = txn.SetSymlinkTarget(child.ino, link);
    if (!status.ok()) {
      return status;
    }
    // Mirror the target/size into the local snapshot for the out params.
    child.symlink_target = link;
    child.attr.size = child.symlink_target.size();
    return Status::OK();
  });

  if (!status.ok()) {
    SWORDFS_LOG_DEBUG << "Symlink: parent=" << parent_ino << " name='" << name << "' failed: " << status.message();
    return status;
  }

  if (out) {
    *out = child;
  }
  return Status::OK();
}

Status MemMetaImpl::Link(InodeID ino, InodeID newparent_ino, std::string_view newname, SwordFsInode *out) {
  utils::ExpectInFiberDomain();
  if (auto status = ValidateNameComponent(newname); !status.ok()) {
    return status;
  }

  SwordFsInode inode;
  Status status = store_.Transact([&](MemMetaTxn &txn) -> Status {
    Status status = txn.LookupInode(ino, &inode);
    if (!status.ok()) {
      return status;
    }

    // Directories cannot be hard-linked (POSIX).
    if (inode.IsDir()) {
      return Status::NotPermitted("cannot hard-link directory");
    }

    SwordFsInode newparent;
    status = txn.LookupInode(newparent_ino, &newparent);
    if (!status.ok()) {
      return status;
    }
    if (!newparent.IsDir()) {
      return Status::NotDirectory("new parent is not a directory");
    }

    return txn.LinkExistingEntry(newparent_ino, newname, ino, &inode);
  });

  if (!status.ok()) {
    SWORDFS_LOG_DEBUG << "Link: ino=" << ino << " parent=" << newparent_ino << " name='" << newname
                      << "' failed: " << status.message();
    return status;
  }
  if (out) {
    *out = inode;
  }
  return Status::OK();
}

Status MemMetaImpl::Readlink(InodeID ino, std::string *target) {
  utils::ExpectInFiberDomain();
  SwordFsInode inode;
  Status status = store_.Transact([&](MemMetaTxn &txn) -> Status {
    Status status = txn.LookupInode(ino, &inode);
    if (!status.ok()) {
      return status;
    }

    if (!inode.IsSymlink()) {
      return Status::InvalidArgument("not a symbolic link");
    }

    *target = inode.symlink_target;
    return Status::OK();
  });

  if (!status.ok()) {
    SWORDFS_LOG_DEBUG << "Readlink: ino " << ino << " failed: " << status.message();
  }
  return status;
}

// ────────────────────────────────────────────────────────────────
// Chunk metadata
// ────────────────────────────────────────────────────────────────

Status MemMetaImpl::CommitChunk(InodeID ino, const std::optional<SwordFsChunk> &expected,
                                const SwordFsChunk &replacement) {
  utils::ExpectInFiberDomain();
  if (!replacement.IsValidForChunkSize(chunk_size_)) {
    return Status::InvalidArgument("replacement chunk descriptor is invalid");
  }
  if (expected.has_value() && !expected->IsValidForChunkSize(chunk_size_)) {
    return Status::InvalidArgument("expected chunk descriptor is invalid");
  }
  return store_.Transact([&](MemMetaTxn &txn) { return txn.CommitChunk(ino, expected, replacement); });
}

Status MemMetaImpl::FindChunk(InodeID ino, ChunkIndex idx, SwordFsChunk *chunk) {
  utils::ExpectInFiberDomain();
  return store_.Transact([&](MemMetaTxn &txn) { return txn.FindChunk(ino, idx, chunk); });
}

Status MemMetaImpl::Truncate(InodeID ino, uint64_t size) {
  utils::ExpectInFiberDomain();
  Status status = store_.Transact([&](MemMetaTxn &txn) { return txn.Truncate(ino, size); });
  if (!status.ok()) {
    SWORDFS_LOG_DEBUG << "Truncate: ino " << ino << " to " << size << " failed: " << status.message();
  }
  return status;
}

// ────────────────────────────────────────────────────────────────
// Volume operations
// ────────────────────────────────────────────────────────────────

Status MemMetaImpl::StatFs(SwordFsStatFs *stbuf) {
  utils::ExpectInFiberDomain();
  if (stbuf == nullptr) {
    return Status::InvalidArgument("statfs output is null");
  }
  *stbuf = {};
  Limits limits = GetLimits();
  stbuf->name_max = limits.max_name_length;
  stbuf->fragment_size = 4096;
  stbuf->block_size = 4096;
  stbuf->blocks = 268435456;  // ~1 TiB
  stbuf->blocks_free = 268435456;
  stbuf->blocks_available = 268435456;
  store_.Transact([&](MemMetaTxn &txn) { stbuf->files = txn.InodeCount(); });
  stbuf->files_free = limits.max_free_inodes;
  return Status::OK();
}

Limits MemMetaImpl::GetLimits() const {
  return kMemLimits;
}

}  // namespace swordfs::metadata
