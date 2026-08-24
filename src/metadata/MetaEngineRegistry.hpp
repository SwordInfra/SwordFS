// Copyright 2026 SwordFS Contributors.
// Licensed under the Apache License, Version 2.0.

#pragma once

#include <memory>
#include <string>
#include <string_view>
#include <unordered_map>

#include "metadata/Types.hpp"
#include "utils/Status.hpp"

namespace swordfs::metadata {

class IMetaEngine;

/// Registry of metadata engines available in this build.
class MetaEngineRegistry {
public:
  using Factory = utils::Status (*)(std::string_view meta_url, std::unique_ptr<IMetaEngine> *out);

  struct Entry {
    Factory factory;
  };

  static MetaEngineRegistry &Instance();

  /// Register a metadata engine. Called at static initialization time.
  void Register(std::string_view name, Factory factory);

  /// Return whether a metadata engine with the given name is registered.
  bool Available(std::string_view name) const;

  /// Create a metadata engine using its registered factory.
  utils::Status Create(std::string_view name, std::string_view meta_url, std::unique_ptr<IMetaEngine> *out) const;

private:
  MetaEngineRegistry() = default;
  std::unordered_map<std::string, Entry> engines_;
};

/// RAII helper for registering a metadata engine at static initialization.
struct RegisterMetaEngine {
  RegisterMetaEngine(std::string_view name, MetaEngineRegistry::Factory factory) {
    MetaEngineRegistry::Instance().Register(name, factory);
  }
};

}  // namespace swordfs::metadata
