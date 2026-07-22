// Copyright 2026 SwordFS Contributors.
// Licensed under the Apache License, Version 2.0.

// SwordFsContext — per-request call context carrying caller identity.
// Populated from fuse_req_ctx(req) at the VFS boundary and passed
// through the internal APIs so that ownership / permission checks
// use the real caller instead of the daemon's credentials.

#pragma once

#define FUSE_USE_VERSION 312
#include <fuse_lowlevel.h>

#include <sys/types.h>

namespace swordfs::utils {

struct SwordFsContext {
  SwordFsContext() = default;
  SwordFsContext(const struct fuse_ctx* ctx)
      : uid(ctx->uid), gid(ctx->gid), pid(ctx->pid), umask(ctx->umask) {}

  uid_t uid = 0;
  gid_t gid = 0;
  pid_t pid = 0;
  mode_t umask = 0;
};

}  // namespace swordfs::utils
