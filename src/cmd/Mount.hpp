// Copyright 2026 SwordFS Contributors.
// Licensed under the Apache License, Version 2.0.

// Mount subcommand public interface.

#pragma once

#include <string>
#include <string_view>
#include <vector>

#include "runtime/MountRuntimeBehavior.hpp"

namespace swordfs::cmd {

namespace detail {

std::vector<std::string> BuildFuseExtras(std::string_view user_opts);

runtime::ImplicitAtimePolicy ParseImplicitAtimePolicy(std::string_view user_opts);

}  // namespace detail

int RunMount();

}  // namespace swordfs::cmd
