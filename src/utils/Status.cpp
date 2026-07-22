// Copyright 2026 SwordFS Contributors.
// Licensed under the Apache License, Version 2.0.

#include "utils/Status.hpp"

#include <cerrno>

namespace swordfs::utils {

int Status::ToErrno() const {
  switch (code_) {
    case kOk:
      return 0;
    case kNotFound:
      return ENOENT;
    case kAlreadyExists:
      return EEXIST;
    case kNotDirectory:
      return ENOTDIR;
    case kInvalidArgument:
      return EINVAL;
    case kNotSupported:
      return ENOSYS;
    case kIOError:
      return EIO;
    case kBusy:
      return EBUSY;
    case kNoSpace:
      return ENOSPC;
    case kPermission:
      return EACCES;
    case kNoMemory:
      return ENOMEM;
    case kInternal:
      return EIO;  // maps to generic I/O error for kernel
    default:
      return EIO;
  }
}

}  // namespace swordfs
