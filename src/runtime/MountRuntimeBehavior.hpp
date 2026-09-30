// Copyright 2026 SwordFS Contributors.
// Licensed under the Apache License, Version 2.0.

#pragma once

#include <cstdint>

namespace swordfs::runtime {

enum class ImplicitAtimePolicy : uint8_t {
  kEnabled,
  kDisabled,
};

class MountRuntimeBehavior {
 public:
  static MountRuntimeBehavior &Instance();

  void Initialize(ImplicitAtimePolicy implicit_atime_policy, bool ioctl_enabled);
  void SetPosixAclEnabled(bool enabled);

  bool ImplicitAtimeUpdatesEnabled() const;
  bool IoctlEnabled() const;
  bool PosixAclEnabled() const;

 private:
  ImplicitAtimePolicy implicit_atime_policy_ = ImplicitAtimePolicy::kEnabled;
  bool ioctl_enabled_ = false;
  bool posix_acl_enabled_ = false;
};

}  // namespace swordfs::runtime
