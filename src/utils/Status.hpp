// Copyright 2026 SwordFS Contributors.
// Licensed under the Apache License, Version 2.0.

// Generic status/error type for internal APIs.
// Each internal layer (Meta, VfsImpl) returns Status;
// only the FUSE callback layer (VfsHookFactory) translates
// the Status into fuse_reply_err / fuse_reply_*.

#pragma once

#include <string>

namespace swordfs::utils {

class Status {
 public:
  enum Code : int {
    kOk = 0,
    kNotFound,         // ENOENT
    kAlreadyExists,    // EEXIST
    kNotDirectory,     // ENOTDIR
    kInvalidArgument,  // EINVAL
    kNotSupported,     // ENOSYS
    kIOError,          // EIO
    kBusy,             // EBUSY
    kNoSpace,          // ENOSPC
    kPermission,       // EPERM / EACCES
    kNoMemory,         // ENOMEM
    kInternal,         // internal / unexpected error
  };

  Status() : code_(kOk) {}
  Status(Code code, std::string msg) : code_(code), msg_(std::move(msg)) {}

  // queries

  bool ok() const { return code_ == kOk; }
  bool IsNotFound() const { return code_ == kNotFound; }
  bool IsAlreadyExists() const { return code_ == kAlreadyExists; }
  bool IsBusy() const { return code_ == kBusy; }
  bool IsNotDirectory() const { return code_ == kNotDirectory; }
  bool IsNotSupported() const { return code_ == kNotSupported; }
  bool IsPermission() const { return code_ == kPermission; }

  Code code() const { return code_; }
  const std::string& message() const { return msg_; }

  /// Maps the internal code to the matching negative errno value, suitable
  /// for fuse_reply_err(req, st.ToErrno()).
  int ToErrno() const;

  // factories

  static Status OK() { return Status(); }
  static Status NotFound(std::string msg) {
    return Status(kNotFound, std::move(msg));
  }
  static Status AlreadyExists(std::string msg) {
    return Status(kAlreadyExists, std::move(msg));
  }
  static Status NotDirectory(std::string msg) {
    return Status(kNotDirectory, std::move(msg));
  }
  static Status InvalidArgument(std::string msg) {
    return Status(kInvalidArgument, std::move(msg));
  }
  static Status NotSupported(std::string msg) {
    return Status(kNotSupported, std::move(msg));
  }
  static Status IOError(std::string msg) {
    return Status(kIOError, std::move(msg));
  }
  static Status Busy(std::string msg) { return Status(kBusy, std::move(msg)); }
  static Status NoSpace(std::string msg) {
    return Status(kNoSpace, std::move(msg));
  }
  static Status Permission(std::string msg) {
    return Status(kPermission, std::move(msg));
  }
  static Status NoMemory(std::string msg) {
    return Status(kNoMemory, std::move(msg));
  }
  static Status Internal(std::string msg) {
    return Status(kInternal, std::move(msg));
  }

 private:
  Code code_;
  std::string msg_;
};

}  // namespace swordfs::utils
