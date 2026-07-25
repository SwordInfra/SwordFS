// Copyright 2026 SwordFS Contributors.
// Licensed under the Apache License, Version 2.0.

#include "fuse/VfsImpl.hpp"

#include <dirent.h>
#include <folly/fibers/FiberManagerInternal.h>

#include "fuse/Limits.hpp"
#include "metadata/Meta.hpp"
#include "metadata/mem/MemMetaImpl.hpp"
#include "storage/IDataEngine.hpp"
#include "utils/ConfigCenter.hpp"
#include "utils/Context.hpp"
#include "utils/Logging.hpp"
#include "utils/Status.hpp"

#define FUSE_USE_VERSION 312
#include <fuse_lowlevel.h>

#include <cerrno>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

using namespace swordfs::utils;

using swordfs::metadata::InodeID;
using swordfs::metadata::SwordFsEntry;

namespace swordfs::fuse {

VfsImpl::VfsImpl() {
  if (ConfigCenter::Instance().vfs_backend() == VfsBackend::kMemory) {
    meta_engine_ = std::make_unique<swordfs::metadata::MemMetaImpl>();
  } else {
    SWORDFS_PROMPT_EXIT << "VFS backend not supported";
    return;
  }
}

VfsImpl::~VfsImpl() = default;

void VfsImpl::set_data_engine(
    std::unique_ptr<swordfs::storage::IDataEngine> data) {
  data_engine_ = std::move(data);
}

void VfsImpl::Init(void* userdata, struct fuse_conn_info* conn) {
  (void)userdata;
  conn->no_interrupt = 1;
  conn->max_write = kMaxWriteSize;
  conn->max_readahead = kMaxReadAheadSize;
  conn->time_gran = kTimeGran;

  // Enable features (only if the kernel supports them).
  if (conn->capable & FUSE_CAP_WRITEBACK_CACHE)
    fuse_set_feature_flag(conn, FUSE_CAP_WRITEBACK_CACHE);
  if (conn->capable & FUSE_CAP_SPLICE_READ)
    fuse_set_feature_flag(conn, FUSE_CAP_SPLICE_READ);
  if (conn->capable & FUSE_CAP_READDIRPLUS)
    fuse_set_feature_flag(conn, FUSE_CAP_READDIRPLUS);
  if (conn->capable & FUSE_CAP_ASYNC_READ)
    fuse_set_feature_flag(conn, FUSE_CAP_ASYNC_READ);
  if (conn->capable & FUSE_CAP_ATOMIC_O_TRUNC)
    fuse_set_feature_flag(conn, FUSE_CAP_ATOMIC_O_TRUNC);
  if (conn->capable & FUSE_CAP_DONT_MASK)
    fuse_set_feature_flag(conn, FUSE_CAP_DONT_MASK);

  // Explicitly disable splice write — write_buf is not yet implemented.
  fuse_unset_feature_flag(conn, FUSE_CAP_SPLICE_WRITE);

  SWORDFS_LOG_INFO << "SwordFS filesystem initialized (mount OK)";
}

void VfsImpl::Destroy(void* userdata) {
  (void)userdata;
  SWORDFS_LOG_INFO << "SwordFS filesystem unmounted";
}

void VfsImpl::Lookup(fuse_req_t req, fuse_ino_t parent, const char* name) {
  folly::fibers::local<SwordFsContext>() = SwordFsContext{fuse_req_ctx(req)};
  InodeID child_ino;
  struct stat attr;
  Status status = meta_engine_->Lookup(parent, name, &child_ino,
                                       &attr);
  if (!status.ok()) {
    fuse_reply_err(req, status.ToErrno());
    return;
  }
  fuse_entry_param entry = {};
  entry.ino = child_ino;
  entry.attr = attr;
  entry.attr_timeout = 1.0;
  entry.entry_timeout = 1.0;
  fuse_reply_entry(req, &entry);
}

void VfsImpl::Forget(fuse_req_t req, fuse_ino_t ino, uint64_t nlookup) {
  folly::fibers::local<SwordFsContext>() = SwordFsContext{fuse_req_ctx(req)};
  meta_engine_->Forget(ino, nlookup);
}

void VfsImpl::Getattr(fuse_req_t req, fuse_ino_t ino,
                      struct fuse_file_info* fi) {
  folly::fibers::local<SwordFsContext>() = SwordFsContext{fuse_req_ctx(req)};
  (void)fi;
  struct stat attr;
  Status status = meta_engine_->GetAttr(ino, &attr);
  if (!status.ok()) {
    fuse_reply_err(req, status.ToErrno());
  } else {
    fuse_reply_attr(req, &attr, 1.0);
  }
}

void VfsImpl::Setattr(fuse_req_t req, fuse_ino_t ino, struct stat* attr,
                      int to_set, struct fuse_file_info* fi) {
  folly::fibers::local<SwordFsContext>() = SwordFsContext{fuse_req_ctx(req)};
  struct stat out_attr;
  Status status = meta_engine_->SetAttr(ino, attr, to_set,
                                        &out_attr);
  if (!status.ok()) {
    fuse_reply_err(req, status.ToErrno());
  } else {
    (void)fi;
    fuse_reply_attr(req, &out_attr, 1.0);
  }
}

void VfsImpl::Readlink(fuse_req_t req, fuse_ino_t ino) {
  (void)ino;
  fuse_reply_err(req, ENOSYS);
}

void VfsImpl::Mknod(fuse_req_t req, fuse_ino_t parent, const char* name,
                    mode_t mode, dev_t rdev) {
  (void)parent;
  (void)name;
  (void)mode;
  (void)rdev;
  fuse_reply_err(req, ENOSYS);
}

void VfsImpl::Mkdir(fuse_req_t req, fuse_ino_t parent, const char* name,
                    mode_t mode) {
  folly::fibers::local<SwordFsContext>() = SwordFsContext{fuse_req_ctx(req)};
  InodeID child_ino;
  struct stat attr;
  Status status = meta_engine_->MkDir(parent, name, mode,
                                      &child_ino, &attr);
  if (!status.ok()) {
    fuse_reply_err(req, status.ToErrno());
    return;
  }
  fuse_entry_param entry = {};
  entry.ino = child_ino;
  entry.attr = attr;
  entry.attr_timeout = 1.0;
  entry.entry_timeout = 1.0;
  fuse_reply_entry(req, &entry);
}

void VfsImpl::Unlink(fuse_req_t req, fuse_ino_t parent, const char* name) {
  folly::fibers::local<SwordFsContext>() = SwordFsContext{fuse_req_ctx(req)};
  Status status = meta_engine_->Unlink(parent, name);
  fuse_reply_err(req, status.ToErrno());
}

void VfsImpl::Rmdir(fuse_req_t req, fuse_ino_t parent, const char* name) {
  folly::fibers::local<SwordFsContext>() = SwordFsContext{fuse_req_ctx(req)};
  Status status = meta_engine_->RmDir(parent, name);
  fuse_reply_err(req, status.ToErrno());
}

void VfsImpl::Symlink(fuse_req_t req, const char* link, fuse_ino_t parent,
                      const char* name) {
  (void)link;
  (void)parent;
  (void)name;
  fuse_reply_err(req, ENOSYS);
}

void VfsImpl::Rename(fuse_req_t req, fuse_ino_t parent, const char* name,
                     fuse_ino_t newparent, const char* newname,
                     unsigned int flags) {
  folly::fibers::local<SwordFsContext>() = SwordFsContext{fuse_req_ctx(req)};
  Status status = meta_engine_->Rename(parent, name, newparent,
                                       newname, flags);
  fuse_reply_err(req, status.ToErrno());
}

void VfsImpl::Link(fuse_req_t req, fuse_ino_t ino, fuse_ino_t newparent,
                   const char* newname) {
  (void)ino;
  (void)newparent;
  (void)newname;
  fuse_reply_err(req, ENOSYS);
}

void VfsImpl::Open(fuse_req_t req, fuse_ino_t ino,
                   struct fuse_file_info* fi) {
  folly::fibers::local<SwordFsContext>() = SwordFsContext{fuse_req_ctx(req)};
  uint64_t fh;
  Status status = meta_engine_->Open(ino, &fh);
  if (!status.ok()) {
    SWORDFS_LOG_ERROR << "Open FAILED: ino=" << ino << " — " << status.message();
    fuse_reply_err(req, status.ToErrno());
    return;
  }
  fi->fh = fh;
  SWORDFS_LOG_DEBUG << "Open: ino=" << ino << " fh=" << fh;
  fuse_reply_open(req, fi);
}

// ────────────────────────────────────────────────────────────────
// Read / Write — chunk-based via data engine
// ────────────────────────────────────────────────────────────────

Status VfsImpl::FlushWriteBuf(uint64_t fh, bool force) {
  auto it = write_bufs_.find(fh);
  if (it == write_bufs_.end()) {
    SWORDFS_LOG_DEBUG << "FlushWriteBuf: fh=" << fh << " — no buffer";
    return Status::OK();
  }
  WriteBuf& wb = it->second;

  if (wb.data.empty()) return Status::OK();

  // When not forced, flush exactly max_chunk_size bytes and keep the
  // remainder for the next chunk.  When forced (Flush/Release/Fsync),
  // flush everything even if below the threshold.
  size_t flush_size;
  if (force) {
    flush_size = wb.data.size();
  } else {
    if (wb.data.size() < wb.max_chunk_size) {
      SWORDFS_LOG_DEBUG << "FlushWriteBuf: fh=" << fh
                        << " buf=" << wb.data.size()
                        << " — below threshold (max=" << wb.max_chunk_size
                        << ")";
      return Status::OK();
    }
    flush_size = wb.max_chunk_size;
  }

  std::string key = std::to_string(wb.ino) + "/" +
                    std::to_string(wb.next_chunk);
  Status status = data_engine_->Put(
      key, std::string_view(wb.data.data(), flush_size));
  if (!status.ok()) {
    SWORDFS_LOG_ERROR << "FlushWriteBuf FAILED: ino=" << wb.ino
                      << " chunk=" << wb.next_chunk
                      << " size=" << flush_size
                      << " — " << status.message();
    return status;
  }

  SWORDFS_LOG_INFO << "FlushWriteBuf: ino=" << wb.ino
                   << " chunk=" << wb.next_chunk
                   << " size=" << flush_size;

  // Update file size from actual write offsets.
  struct stat attr;
  if (meta_engine_->GetAttr(wb.ino, &attr).ok() &&
      wb.max_write_end > attr.st_size) {
    struct stat new_attr = {};
    new_attr.st_size = wb.max_write_end;
    meta_engine_->SetAttr(wb.ino, &new_attr, FUSE_SET_ATTR_SIZE, nullptr);
  }

  // Remove only the flushed bytes; keep the remainder for next chunk.
  wb.data.erase(wb.data.begin(), wb.data.begin() + flush_size);
  wb.next_chunk++;
  return Status::OK();
}

void VfsImpl::Read(fuse_req_t req, fuse_ino_t ino, size_t size, off_t off,
                   uint64_t fh) {
  folly::fibers::local<SwordFsContext>() = SwordFsContext{fuse_req_ctx(req)};
  (void)fh;

  std::string data;
  Status status = HandleRead(ino, size, off, &data);
  if (!status.ok()) {
    SWORDFS_LOG_ERROR << "Read failed: ino=" << ino << " offset=" << off
                      << " — " << status.message();
    fuse_reply_err(req, status.ToErrno());
    return;
  }

  SWORDFS_LOG_DEBUG << "Read: ino=" << ino << " offset=" << off
                    << " nread=" << data.size();
  fuse_reply_buf(req, data.data(), data.size());
}

// ────────────────────────────────────────────────────────────────
// HandleRead — core read logic (no FUSE dependency, testable)
// ────────────────────────────────────────────────────────────────

Status VfsImpl::HandleRead(InodeID ino, size_t size, off_t off,
                           std::string* out) {
  if (!data_engine_) {
    return Status::Internal("no data engine configured");
  }

  size_t chunk_sz = data_engine_->Limits().max_chunk_size;
  out->clear();
  out->reserve(size);

  // Locate any active write buffer for this inode — unflushed data
  // must take precedence over storage.
  WriteBuf* wb = nullptr;
  for (auto& [fh, candidate] : write_bufs_) {
    if (candidate.ino == ino && !candidate.data.empty()) {
      wb = &candidate;
      break;
    }
  }
  // If a buffer exists, its data occupies file offsets
  // [next_chunk * max_chunk_size, next_chunk * max_chunk_size + data.size()).
  off_t buf_start = 0;
  off_t buf_end = 0;
  if (wb) {
    buf_start = static_cast<off_t>(wb->next_chunk) *
                static_cast<off_t>(wb->max_chunk_size);
    buf_end = buf_start + static_cast<off_t>(wb->data.size());
  }

  size_t remaining = size;
  off_t cur_off = off;

  while (remaining > 0) {
    // ── Serve from write buffer if the current offset falls within it ──
    if (wb && cur_off >= buf_start && cur_off < buf_end) {
      size_t buf_off = static_cast<size_t>(cur_off - buf_start);
      size_t buf_avail = static_cast<size_t>(buf_end - cur_off);
      size_t n = std::min(remaining, buf_avail);
      out->append(wb->data.data() + buf_off, n);
      remaining -= n;
      cur_off += static_cast<off_t>(n);
      continue;
    }

    // ── Serve from storage ──
    off_t chunk_idx = cur_off / static_cast<off_t>(chunk_sz);
    size_t chunk_off_val = static_cast<size_t>(
        cur_off % static_cast<off_t>(chunk_sz));
    std::string key =
        std::to_string(ino) + "/" + std::to_string(chunk_idx);

    size_t chunk_actual = 0;
    if (!data_engine_->Head(key, &chunk_actual) ||
        chunk_off_val >= chunk_actual) {
      break;  // EOF — no more chunks (and nothing in buffer)
    }

    size_t to_read = std::min(remaining, chunk_actual - chunk_off_val);
    std::string chunk_data;
    Status status =
        data_engine_->Get(key, &chunk_data, chunk_off_val, to_read);
    if (!status.ok()) {
      return status;
    }

    out->append(chunk_data);
    remaining -= chunk_data.size();
    cur_off += chunk_data.size();

    // If the engine returned fewer bytes than requested we've hit EOF.
    if (chunk_data.size() < to_read) break;
  }

  return Status::OK();
}

void VfsImpl::Write(fuse_req_t req, fuse_ino_t ino, const char* buf,
                    size_t size, off_t off, uint64_t fh) {
  folly::fibers::local<SwordFsContext>() = SwordFsContext{fuse_req_ctx(req)};

  if (!data_engine_) {
    SWORDFS_LOG_ERROR << "Write: no data engine configured (ino=" << ino << ")";
    fuse_reply_err(req, EIO);
    return;
  }

  // Get or create write buffer for this file handle.
  auto& wb = write_bufs_[fh];
  if (wb.ino == 0) {
    wb.ino = ino;
    wb.max_chunk_size = data_engine_->Limits().max_chunk_size;
  }

  // Append data to buffer.
  wb.data.insert(wb.data.end(), buf, buf + size);
  off_t end = off + static_cast<off_t>(size);
  if (end > wb.max_write_end) wb.max_write_end = end;

  SWORDFS_LOG_DEBUG << "Write buffered: ino=" << ino << " offset=" << off
                    << " size=" << size
                    << " buf_total=" << wb.data.size();

  // Flush if buffer is full.
  if (wb.data.size() >= wb.max_chunk_size) {
    Status st = FlushWriteBuf(fh, false);
    if (!st.ok()) {
      fuse_reply_err(req, st.ToErrno());
      return;
    }
  }

  fuse_reply_write(req, size);
}

void VfsImpl::Flush(fuse_req_t req, fuse_ino_t ino,
                    uint64_t fh) {
  SWORDFS_LOG_DEBUG << "Flush: ino=" << ino << " fh=" << fh;
  Status st = FlushWriteBuf(fh, /*force=*/true);
  fuse_reply_err(req, st.ok() ? 0 : st.ToErrno());
}

void VfsImpl::Release(fuse_req_t req, fuse_ino_t ino,
                      uint64_t fh) {
  folly::fibers::local<SwordFsContext>() = SwordFsContext{fuse_req_ctx(req)};

  // Flush any remaining buffered data before releasing.
  Status st = FlushWriteBuf(fh, /*force=*/true);
  if (!st.ok()) {
    SWORDFS_LOG_ERROR << "Release: flush failed for fh=" << fh
                      << " — " << st.message();
  }

  write_bufs_.erase(fh);

  st = meta_engine_->Release(fh);
  if (!st.ok()) {
    SWORDFS_LOG_ERROR << "Release FAILED: ino=" << ino << " fh=" << fh
                      << " — " << st.message();
  } else {
    SWORDFS_LOG_DEBUG << "Release: ino=" << ino << " fh=" << fh;
  }
  fuse_reply_err(req, st.ToErrno());
}

void VfsImpl::Fsync(fuse_req_t req, fuse_ino_t ino, int datasync,
                    uint64_t fh) {
  SWORDFS_LOG_DEBUG << "Fsync: ino=" << ino << " datasync=" << datasync
                    << " fh=" << fh;
  Status st = FlushWriteBuf(fh, /*force=*/true);
  fuse_reply_err(req, st.ok() ? 0 : st.ToErrno());
}

void VfsImpl::Opendir(fuse_req_t req, fuse_ino_t ino,
                      struct fuse_file_info* fi) {
  folly::fibers::local<SwordFsContext>() = SwordFsContext{fuse_req_ctx(req)};
  uint64_t fh;
  Status status = meta_engine_->OpenDir(ino, &fh);
  if (!status.ok()) {
    fuse_reply_err(req, status.ToErrno());
    return;
  }
  fi->fh = fh;
  fuse_reply_open(req, fi);
}

// Common implementation for Readdir and Readdirplus.
// 1. Read directory entries + prepend "." / ".."
// 2. Two-pass buffer construction: pass 1 calculates sizes,
//    pass 2 fills the buffer with correct `off` values.
// The `add_entry` callback is the only difference: fuse_add_direntry
// for Readdir, fuse_add_direntry_plus for Readdirplus.
template <typename F>
static void ReaddirCommon(fuse_req_t req, fuse_ino_t ino, size_t size,
                          off_t off,
                          std::unique_ptr<swordfs::metadata::IMetaEngine>& meta,
                          F&& add_entry) {
  using swordfs::metadata::SwordFsEntry;

  std::vector<SwordFsEntry> entries;
  Status st = meta->ReadDir(ino, &entries);
  if (!st.ok()) {
    fuse_reply_err(req, st.ToErrno());
    return;
  }

  // "." and ".." required by FUSE low-level API.
  entries.insert(entries.begin(), {".", DT_DIR, ino});
  entries.insert(entries.begin() + 1,
                 {"..", DT_DIR, (ino == FUSE_ROOT_ID) ? ino : 0});

  std::vector<size_t> sizes(entries.size());
  size_t cap = 0;
  for (size_t i = 0; i < entries.size(); ++i) {
    sizes[i] = add_entry(req, nullptr, 0, entries[i], 0);
    cap += sizes[i];
  }

  char* buf = static_cast<char*>(std::malloc(cap));
  if (!buf) {
    fuse_reply_err(req, ENOMEM);
    return;
  }

  size_t pos = 0;
  for (size_t i = 0; i < entries.size() && pos < cap; ++i) {
    size_t n = add_entry(req, buf + pos, cap - pos,
                         entries[i], pos + sizes[i]);
    if (n > cap - pos) break;
    pos += n;
  }

  if (static_cast<size_t>(off) < pos)
    fuse_reply_buf(req, buf + off, std::min(pos - off, size));
  else
    fuse_reply_buf(req, nullptr, 0);
  std::free(buf);
}

void VfsImpl::Readdir(fuse_req_t req, fuse_ino_t ino, size_t size,
                      off_t off, struct fuse_file_info* fi) {
  (void)fi;
  folly::fibers::local<SwordFsContext>() = SwordFsContext{fuse_req_ctx(req)};

  ReaddirCommon(req, ino, size, off, meta_engine_,
                [](fuse_req_t req, char* buf, size_t bufsize,
                   const swordfs::metadata::SwordFsEntry& e, off_t off) {
                  struct stat st = {};
                  st.st_ino = e.ino;
                  st.st_mode = e.type << 12;
                  return fuse_add_direntry(req, buf, bufsize, e.name.c_str(), &st, off);
                });
}

void VfsImpl::Releasedir(fuse_req_t req, fuse_ino_t ino,
                         struct fuse_file_info* fi) {
  folly::fibers::local<SwordFsContext>() = SwordFsContext{fuse_req_ctx(req)};
  Status status = meta_engine_->ReleaseDir(fi->fh);
  (void)ino;
  fuse_reply_err(req, status.ToErrno());
}

void VfsImpl::Fsyncdir(fuse_req_t req, fuse_ino_t ino, int datasync,
                       struct fuse_file_info* fi) {
  (void)ino;
  (void)datasync;
  (void)fi;
  fuse_reply_err(req, ENOSYS);
}

void VfsImpl::Statfs(fuse_req_t req, fuse_ino_t ino) {
  (void)ino;
  folly::fibers::local<SwordFsContext>() = SwordFsContext{fuse_req_ctx(req)};
  struct statvfs stbuf;
  Status status = meta_engine_->StatFs(&stbuf);
  if (!status.ok()) {
    fuse_reply_err(req, status.ToErrno());
  } else {
    fuse_reply_statfs(req, &stbuf);
  }
}

void VfsImpl::Setxattr(fuse_req_t req, fuse_ino_t ino, const char* name,
                       const char* value, size_t size, int flags) {
  (void)ino;
  (void)name;
  (void)value;
  (void)size;
  (void)flags;
  fuse_reply_err(req, ENOSYS);
}

void VfsImpl::Getxattr(fuse_req_t req, fuse_ino_t ino, const char* name,
                       size_t size) {
  (void)ino;
  (void)name;
  (void)size;
  fuse_reply_err(req, ENOSYS);
}

void VfsImpl::Listxattr(fuse_req_t req, fuse_ino_t ino, size_t size) {
  (void)ino;
  (void)size;
  fuse_reply_err(req, ENOSYS);
}

void VfsImpl::Removexattr(fuse_req_t req, fuse_ino_t ino, const char* name) {
  (void)ino;
  (void)name;
  fuse_reply_err(req, ENOSYS);
}

void VfsImpl::Access(fuse_req_t req, fuse_ino_t ino, int mask) {
  folly::fibers::local<SwordFsContext>() = SwordFsContext{fuse_req_ctx(req)};
  Status status = meta_engine_->Access(ino, mask);
  fuse_reply_err(req, status.ToErrno());
}

void VfsImpl::Create(fuse_req_t req, fuse_ino_t parent, const char* name,
                     mode_t mode, struct fuse_file_info* fi) {
  folly::fibers::local<SwordFsContext>() = SwordFsContext{fuse_req_ctx(req)};
  InodeID child_ino;
  struct stat attr;
  Status status = meta_engine_->Create(parent, name, mode, &child_ino, &attr);
  if (!status.ok()) {
    SWORDFS_LOG_ERROR << "Create FAILED: parent=" << parent << " name='" << name
                      << "' — " << status.message();
    fuse_reply_err(req, status.ToErrno());
    return;
  }
  uint64_t fh;
  Status status2 = meta_engine_->Open(child_ino, &fh);
  if (!status2.ok()) {
    SWORDFS_LOG_ERROR << "Create: Open FAILED: ino=" << child_ino
                      << " — " << status2.message();
    fuse_reply_err(req, status2.ToErrno());
    return;
  }
  fi->fh = fh;
  fuse_entry_param entry = {};
  entry.ino = child_ino;
  entry.attr = attr;
  entry.attr_timeout = 1.0;
  entry.entry_timeout = 1.0;
  SWORDFS_LOG_INFO << "Create: ino=" << child_ino << " fh=" << fh
                   << " name='" << name << "'";
  fuse_reply_create(req, &entry, fi);
}

void VfsImpl::Ioctl(fuse_req_t req, fuse_ino_t ino, int cmd, void* arg,
                    struct fuse_file_info* fi, unsigned flags,
                    const void* in_buf, size_t in_bufsz, size_t out_bufsz) {
  (void)ino;
  (void)cmd;
  (void)arg;
  (void)fi;
  (void)flags;
  (void)in_buf;
  (void)in_bufsz;
  (void)out_bufsz;
  fuse_reply_err(req, ENOSYS);
}

void VfsImpl::RetrieveReply(fuse_req_t req, void* cookie, fuse_ino_t ino,
                            off_t offset, struct fuse_bufvec* bufv) {
  (void)cookie;
  (void)ino;
  (void)offset;
  (void)bufv;
  fuse_reply_err(req, ENOSYS);
}

void VfsImpl::ForgetMulti(fuse_req_t req, size_t count,
                          struct fuse_forget_data* forgets) {
  (void)count;
  (void)forgets;
  fuse_reply_none(req);
}

void VfsImpl::Flock(fuse_req_t req, fuse_ino_t ino,
                    struct fuse_file_info* fi, int op) {
  (void)ino;
  (void)fi;
  (void)op;
  fuse_reply_err(req, ENOSYS);
}

void VfsImpl::Fallocate(fuse_req_t req, fuse_ino_t ino, int mode,
                        off_t offset, off_t length,
                        struct fuse_file_info* fi) {
  (void)ino;
  (void)mode;
  (void)offset;
  (void)length;
  (void)fi;
  fuse_reply_err(req, ENOSYS);
}

void VfsImpl::Readdirplus(fuse_req_t req, fuse_ino_t ino, size_t size,
                          off_t off, struct fuse_file_info* fi) {
  (void)fi;
  folly::fibers::local<SwordFsContext>() = SwordFsContext{fuse_req_ctx(req)};

  ReaddirCommon(req, ino, size, off, meta_engine_,
                [this](fuse_req_t req, char* buf, size_t bufsize,
                       const swordfs::metadata::SwordFsEntry& e, off_t off) {
                  struct stat attr = {};
                  if (e.ino != 0) meta_engine_->GetAttr(e.ino, &attr);
                  struct fuse_entry_param ep = {};
                  ep.ino = e.ino;
                  ep.attr = attr;
                  ep.attr.st_ino = e.ino;
                  ep.attr.st_mode = e.type << 12;
                  ep.attr_timeout = 1.0;
                  ep.entry_timeout = 1.0;
                  return fuse_add_direntry_plus(req, buf, bufsize,
                                                e.name.c_str(), &ep, off);
                });
}

void VfsImpl::Lseek(fuse_req_t req, fuse_ino_t ino, off_t off, int whence,
                    struct fuse_file_info* fi) {
  (void)ino;
  (void)off;
  (void)whence;
  (void)fi;
  fuse_reply_err(req, ENOSYS);
}

void VfsImpl::Tmpfile(fuse_req_t req, fuse_ino_t parent, mode_t mode,
                      struct fuse_file_info* fi) {
  (void)parent;
  (void)mode;
  (void)fi;
  fuse_reply_err(req, ENOSYS);
}

void VfsImpl::Statx(fuse_req_t req, fuse_ino_t ino, int flags, int mask,
                    struct fuse_file_info* fi) {
  (void)ino;
  (void)flags;
  (void)mask;
  (void)fi;
  fuse_reply_err(req, ENOSYS);
}

}  // namespace swordfs::fuse
