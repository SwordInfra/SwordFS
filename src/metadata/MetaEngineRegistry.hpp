// Copyright 2026 SwordFS Contributors.
// Licensed under the Apache License, Version 2.0.

#pragma once

#include <memory>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

#include "metadata/Types.hpp"
#include "utils/Status.hpp"

namespace swordfs::metadata {

class IMetaEngine;

/// Registry of metadata engines available in this build.
class MetaEngineRegistry {
public:
  using Factory = utils::Status (*)(std::string_view meta_url, std::unique_ptr<IMetaEngine> *out);
  using LimitsProvider = Limits (*)();

  struct Entry {
    Factory factory;
    LimitsProvider limits;
  };

  static MetaEngineRegistry &Instance();

  /// Register a metadata engine. Called at static initialization time.
  void Register(std::string_view name, Factory factory, LimitsProvider limits);

  /// Return whether a metadata engine with the given name is registered.
  bool Available(std::string_view name) const;

  /// Create a metadata engine using its registered factory.
  utils::Status Create(std::string_view name, std::string_view meta_url, std::unique_ptr<IMetaEngine> *out) const;

  /// Return the filesystem limits provided by a registered engine.
  Limits GetLimits(std::string_view name) const;

  /// Return the names of all registered metadata engines.
  std::vector<std::string> Names() const;

private:
  MetaEngineRegistry() = default;
  std::unordered_map<std::string, Entry> engines_;
};

/// RAII helper for registering a metadata engine at static initialization.
struct RegisterMetaEngine {
  RegisterMetaEngine(std::string_view name, MetaEngineRegistry::Factory factory,
                     MetaEngineRegistry::LimitsProvider limits) {
    MetaEngineRegistry::Instance().Register(name, factory, limits);
  }
};

}  // namespace swordfs::metadata
