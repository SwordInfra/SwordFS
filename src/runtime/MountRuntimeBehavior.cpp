// Copyright 2026 SwordFS Contributors.
// Licensed under the Apache License, Version 2.0.

#include "runtime/MountRuntimeBehavior.hpp"

#include "utils/ExecutionDomain.hpp"

namespace swordfs::runtime {

MountRuntimeBehavior &MountRuntimeBehavior::Instance() {
  static MountRuntimeBehavior behavior;
  return behavior;
}

void MountRuntimeBehavior::Initialize(ImplicitAtimePolicy implicit_atime_policy) {
  utils::ExpectInThreadDomain();
  implicit_atime_policy_ = implicit_atime_policy;
}

bool MountRuntimeBehavior::ImplicitAtimeUpdatesEnabled() const {
  return implicit_atime_policy_ == ImplicitAtimePolicy::kEnabled;
}

}  // namespace swordfs::runtime
