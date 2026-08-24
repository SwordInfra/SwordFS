// Copyright 2026 SwordFS Contributors.
// Licensed under the Apache License, Version 2.0.

#pragma once

#include <string>
#include <string_view>
#include <unordered_set>

namespace swordfs::metadata {

/// Registry of metadata engines available in this build.
class MetaEngineRegistry {
public:
  static MetaEngineRegistry &Instance();

  /// Register a metadata engine. Called at static initialization time.
  void Register(std::string_view name);

  /// Return whether a metadata engine with the given name is registered.
  bool Available(std::string_view name) const;

private:
  MetaEngineRegistry() = default;
  std::unordered_set<std::string> engines_;
};

/// RAII helper for registering a metadata engine at static initialization.
struct RegisterMetaEngine {
  explicit RegisterMetaEngine(std::string_view name) {
    MetaEngineRegistry::Instance().Register(name);
  }
};

}  // namespace swordfs::metadata
