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
    case kIsDirectory:
      return EISDIR;
    case kInvalidArgument:
      return EINVAL;
    case kMalformed:
      return EIO;
    case kNotSupported:
      return ENOSYS;
    case kIOError:
      return EIO;
    case kBusy:
      return EBUSY;
    case kNotEmpty:
      return ENOTEMPTY;
    case kNoSpace:
      return ENOSPC;
    case kNotPermitted:
      return EPERM;
    case kPermission:
      return EACCES;
    case kNoMemory:
      return ENOMEM;
    case kNameTooLong:
      return ENAMETOOLONG;
    default:
      return EIO;
  }
}

}  // namespace swordfs::utils
