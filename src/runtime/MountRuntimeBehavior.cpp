// Copyright 2026 SwordFS Contributors.
// Licensed under the Apache License, Version 2.0.

#include "runtime/MountRuntimeBehavior.hpp"

#include "utils/ExecutionDomain.hpp"

namespace swordfs::runtime {

MountRuntimeBehavior &MountRuntimeBehavior::Instance() {
  static MountRuntimeBehavior behavior;
  return behavior;
}

void MountRuntimeBehavior::Initialize(ImplicitAtimePolicy implicit_atime_policy, bool ioctl_enabled) {
  utils::ExpectInThreadDomain();
  implicit_atime_policy_ = implicit_atime_policy;
  ioctl_enabled_ = ioctl_enabled;
}

bool MountRuntimeBehavior::ImplicitAtimeUpdatesEnabled() const {
  return implicit_atime_policy_ == ImplicitAtimePolicy::kEnabled;
}

bool MountRuntimeBehavior::IoctlEnabled() const {
  return ioctl_enabled_;
}

}  // namespace swordfs::runtime
