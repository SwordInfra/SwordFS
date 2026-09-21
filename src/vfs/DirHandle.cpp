// Copyright 2026 SwordFS Contributors.
// Licensed under the Apache License, Version 2.0.

#include "vfs/DirHandle.hpp"

#include <glog/logging.h>

#include <algorithm>
#include <optional>
#include <utility>
#include <vector>

#include "vfs/FileReadWriter.hpp"
#include "vfs/Handle.hpp"
#include "vfs/InodeHandle.hpp"
#include "volume/VolumeImpl.hpp"

namespace swordfs::vfs {

utils::Status DirHandle::Open(metadata::InodeID ino, std::shared_ptr<DirHandle> *out) {
  if (out == nullptr) {
    return utils::Status::InvalidArgument("directory handle output is null");
  }

  metadata::DirIteratorPtr iterator;
  auto status = volume::VolumeImpl::Instance().meta_engine()->OpenDir(ino, &iterator);
  if (!status.ok()) {
    return status;
  }

  auto handle = std::make_shared<DirHandle>(std::move(iterator));
  HandleManager::Instance().Register(handle);
  *out = std::move(handle);
  return utils::Status::OK();
}

utils::Status DirHandle::Release() {
  HandleManager::Instance().Unregister(fh());
  return utils::Status::OK();
}

utils::Status DirHandle::ReadDir(off_t off, size_t size, const DirEntryEncoder &encoder, std::string *out) {
  std::lock_guard<utils::FiberMutex> lock(mutex_);
  CHECK(out != nullptr);
  out->clear();
  if (size == 0) {
    return utils::Status::OK();
  }

  auto status = iterator_->Seek(static_cast<uint64_t>(off));
  if (!status.ok()) {
    return status;
  }

  while (out->size() < size) {
    metadata::SwordFsEntry entry;
    uint64_t next_off = 0;
    status = iterator_->Peek(&entry, &next_off);
    if (status.IsEndOfDirectory()) {
      break;
    }
    if (!status.ok()) {
      return status;
    }

    const size_t available = size - out->size();
    const size_t required = encoder.CalSpace(entry, static_cast<off_t>(next_off));
    if (required > available) {
      if (out->empty()) {
        return utils::Status::NoMemory("readdir entry does not fit in buffer");
      }
      break;
    }

    encoder.Encode(entry, static_cast<off_t>(next_off), required, out);
    iterator_->Advance();
  }
  return utils::Status::OK();
}

utils::Status DirHandle::ReadDirPlus(off_t off, size_t size, const DirEntryPlusEncoder &encoder, std::string *out) {
  constexpr size_t kAttrBatchSize = 128;

  struct Candidate {
    metadata::SwordFsEntry entry;
    off_t next_off;
    size_t required;
  };

  std::lock_guard<utils::FiberMutex> lock(mutex_);
  CHECK(out != nullptr);
  out->clear();
  if (size == 0) {
    return utils::Status::OK();
  }

  uint64_t current_cookie = static_cast<uint64_t>(off);
  auto status = iterator_->Seek(current_cookie);
  if (!status.ok()) {
    return status;
  }

  while (out->size() < size) {
    std::vector<Candidate> candidates;
    std::vector<metadata::InodeID> inode_ids;
    candidates.reserve(kAttrBatchSize);
    inode_ids.reserve(kAttrBatchSize);
    bool end_of_directory = false;
    std::optional<uint64_t> blocked_cookie;
    size_t blocked_required = 0;
    size_t reserved = 0;

    while (candidates.size() < kAttrBatchSize) {
      metadata::SwordFsEntry entry;
      uint64_t next_cookie = 0;
      status = iterator_->Peek(&entry, &next_cookie);
      if (status.IsEndOfDirectory()) {
        end_of_directory = true;
        break;
      }
      if (!status.ok()) {
        return status;
      }

      const size_t required = encoder.CalSpace(entry, static_cast<off_t>(next_cookie));
      const size_t available = size - out->size();
      if (required > available - reserved) {
        if (candidates.empty()) {
          if (out->empty()) {
            return utils::Status::NoMemory("readdirplus entry does not fit in buffer");
          }
          return utils::Status::OK();
        }
        blocked_cookie = current_cookie;
        blocked_required = required;
        break;
      }

      reserved += required;
      candidates.push_back({std::move(entry), static_cast<off_t>(next_cookie), required});
      inode_ids.push_back(candidates.back().entry.ino);
      iterator_->Advance();
      current_cookie = next_cookie;
    }

    if (candidates.empty()) {
      break;
    }

    // Preserve #212's tracked-inode attribute invariant: persistent metadata
    // and the local live overlay must be one coherent read with respect to
    // local writes/flushes/truncates/setattr-size. Acquire only handles that
    // are already tracked by this mount, deduplicate hard-linked inode IDs,
    // and use a stable lock order before issuing the one backend batch read.
    std::vector<std::pair<metadata::InodeID, std::shared_ptr<InodeHandle>>> tracked;
    tracked.reserve(inode_ids.size());
    for (const metadata::InodeID candidate_ino : inode_ids) {
      auto handle = InodeHandleManager::Instance().Get(candidate_ino, false);
      if (handle) {
        tracked.emplace_back(candidate_ino, std::move(handle));
      }
    }
    std::sort(tracked.begin(), tracked.end(),
              [](const auto &left, const auto &right) { return left.first < right.first; });
    tracked.erase(std::unique(tracked.begin(), tracked.end(),
                              [](const auto &left, const auto &right) { return left.first == right.first; }),
                  tracked.end());

    std::vector<LiveAttrGuard> live_attr_guards;
    live_attr_guards.reserve(tracked.size());
    for (const auto &[tracked_ino, handle] : tracked) {
      (void)tracked_ino;
      live_attr_guards.push_back(handle->LockLiveAttr());
    }

    std::vector<std::optional<metadata::SwordFsInode>> inodes;
    status = volume::VolumeImpl::Instance().meta_engine()->GetInodes(inode_ids, &inodes);
    if (!status.ok()) {
      return status;
    }
    for (size_t i = 0; i < candidates.size(); ++i) {
      auto &maybe_inode = inodes[i];
      if (!maybe_inode.has_value()) {
        // The name was observed before a concurrent namespace mutation removed
        // the inode. Do not fabricate attrs or fail unrelated entries in the batch.
        continue;
      }

      auto &inode = maybe_inode.value();
      if (inode.ino != candidates[i].entry.ino) {
        return utils::Status::Malformed("directory entry inode identity mismatch");
      }
      auto tracked_it = std::lower_bound(tracked.begin(), tracked.end(), inode.ino,
                                         [](const auto &item, metadata::InodeID ino) { return item.first < ino; });
      if (tracked_it != tracked.end() && tracked_it->first == inode.ino) {
        const size_t guard_index = static_cast<size_t>(std::distance(tracked.begin(), tracked_it));
        live_attr_guards[guard_index].Apply(inode);
      }

      CHECK_LE(candidates[i].required, size - out->size());
      encoder.Encode(candidates[i].entry, inode, candidates[i].next_off, candidates[i].required, out);
    }

    if (end_of_directory) {
      break;
    }
    if (blocked_cookie.has_value()) {
      // Missing inodes may have released reply capacity that was reserved while
      // collecting this batch. If the blocked entry now fits, reset Peek's
      // pending state to its cookie and continue without forcing the caller to
      // issue an empty/short retry. Otherwise the last encoded next_off is the
      // correct continuation point for the next FUSE request.
      if (blocked_required > size - out->size()) {
        if (out->empty()) {
          return utils::Status::NoMemory("readdirplus entry does not fit in buffer");
        }
        return utils::Status::OK();
      }
      current_cookie = *blocked_cookie;
      status = iterator_->Seek(current_cookie);
      if (!status.ok()) {
        return status;
      }
    }
  }
  return utils::Status::OK();
}

}  // namespace swordfs::vfs
