// Copyright 2026 SwordFS Contributors.
// Licensed under the Apache License, Version 2.0.

// StorageRegistry — maps backend names to IDataEngine factory functions.
//
// Each storage backend registers itself at static-init time via the
// Register() helper.  ConfigCenter queries Available() to validate
// the user's backend choice and Create() to instantiate it.

#pragma once

#include <functional>
#include <memory>
#include <string>
#include <string_view>
#include <unordered_map>

#include "storage/IDataEngine.hpp"

namespace swordfs::storage {

/// Registry of available IDataEngine backends.
class StorageRegistry {
 public:
  using Factory = std::function<std::unique_ptr<IDataEngine>()>;

  static StorageRegistry& Instance();

  /// Register a backend.  Called at static-init time.
  void Register(std::string_view name, Factory factory);

  /// Return true if a backend with the given name is registered.
  bool Available(std::string_view name) const;

  /// Create an engine instance by name.  Returns nullptr if unknown.
  std::unique_ptr<IDataEngine> Create(std::string_view name) const;

 private:
  StorageRegistry() = default;
  std::unordered_map<std::string, Factory> factories_;
};

/// RAII helper: registers a backend at static-init time.
struct RegisterBackend {
  RegisterBackend(std::string_view name, StorageRegistry::Factory factory) {
    StorageRegistry::Instance().Register(name, std::move(factory));
  }
};

}  // namespace swordfs::storage
