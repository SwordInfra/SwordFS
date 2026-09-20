// Copyright 2026 SwordFS Contributors.
// Licensed under the Apache License, Version 2.0.

// Mount subcommand public interface.

#pragma once

#include <string>
#include <string_view>
#include <vector>

namespace swordfs::cmd {

namespace detail {

std::vector<std::string> BuildFuseExtras(std::string_view user_opts);

}  // namespace detail

int RunMount();

}  // namespace swordfs::cmd
