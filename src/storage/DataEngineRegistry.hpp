// Copyright 2026 SwordFS Contributors.
// Licensed under the Apache License, Version 2.0.

#pragma once

#include <memory>
#include <string>
#include <string_view>
#include <unordered_map>

#include "utils/Status.hpp"

namespace swordfs::storage {

class IDataEngine;
}

namespace swordfs::storage {

/// Registry of data engines available in this build.
class DataEngineRegistry {
 public:
  using Factory = utils::Status (*)(std::unique_ptr<IDataEngine> *out);

  static DataEngineRegistry &Instance();

  /// Register a data engine factory. Called at static initialization time.
  void Register(std::string_view name, Factory factory);

  /// Return whether a data engine with the given scheme is registered.
  bool Available(std::string_view name) const;

  /// Create a data engine using its registered factory.
  utils::Status Create(std::string_view name, std::unique_ptr<IDataEngine> *out) const;

 private:
  DataEngineRegistry() = default;
  std::unordered_map<std::string, Factory> factories_;
};

/// RAII helper for registering a data engine at static initialization.
struct RegisterDataEngine {
  RegisterDataEngine(std::string_view name, DataEngineRegistry::Factory factory) {
    DataEngineRegistry::Instance().Register(name, factory);
  }
};

}  // namespace swordfs::storage
