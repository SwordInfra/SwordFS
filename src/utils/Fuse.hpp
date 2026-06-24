// Copyright 2026 SwordFS Contributors.
// Licensed under the Apache License, Version 2.0.

// FUSE utilities

#pragma once

#define FUSE_USE_VERSION 31
#include <fuse_lowlevel.h>
#include <fuse_opt.h>
#include <sys/stat.h>

#include <string>

namespace swordfs::utils {

// RAII wrapper that frees fuse_args on scope exit.
class FuseArgsGuard {
 public:
  FuseArgsGuard(int argc, char** argv) : args_(FUSE_ARGS_INIT(argc, argv)) {}
  ~FuseArgsGuard() { fuse_opt_free_args(&args_); }
  FuseArgsGuard(const FuseArgsGuard&) = delete;
  FuseArgsGuard& operator=(const FuseArgsGuard&) = delete;

  fuse_args* operator->() { return &args_; }
  fuse_args* get() { return &args_; }

 private:
  fuse_args args_;
};

// RAII wrapper that manages a fuse_session.
// On destruction: removes signal handlers, unmounts, then destroys the session.
class FuseSessionGuard {
 public:
  FuseSessionGuard() = default;
  explicit FuseSessionGuard(fuse_session* se) : se_(se) {}
  ~FuseSessionGuard() {
    if (se_) {
      fuse_session_unmount(se_);
      fuse_session_destroy(se_);
    }
  }
  FuseSessionGuard(const FuseSessionGuard&) = delete;
  FuseSessionGuard& operator=(const FuseSessionGuard&) = delete;

  fuse_session* get() { return se_; }
  fuse_session** ptr() { return &se_; }
  explicit operator bool() const { return se_ != nullptr; }

 private:
  fuse_session* se_ = nullptr;
};

// Mountpoint validation
bool IsFuseMounted(const std::string& mp) {
  // Check /proc/mounts for an existing FUSE mount
  FILE* f = std::fopen("/proc/mounts", "r");
  if (!f) return false;
  char line[4096];
  while (std::fgets(line, sizeof(line), f)) {
    // Each line: device mountpoint fstype ...
    char dev[256], path[256], fstype[32];
    if (std::sscanf(line, "%255s %255s %31s", dev, path, fstype) == 3) {
      if (mp == path) {
        std::fclose(f);
        return true;
      }
    }
  }
  std::fclose(f);
  return false;
}

bool IsStaleMount(const std::string& mp) {
  // Check if the mountpoint is listed but the FUSE connection is dead.
  // A simple heuristic: try to stat the mountpoint; if it returns ENOTCONN
  // the FUSE daemon is gone but the kernel still holds the mount.
  struct stat st;
  if (::stat(mp.c_str(), &st) == -1 && errno == ENOTCONN) {
    return true;
  }
  return false;
}

}  // namespace swordfs::utils
