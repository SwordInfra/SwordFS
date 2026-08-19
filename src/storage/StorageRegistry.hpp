// Copyright 2026 SwordFS Contributors.
// Licensed under the Apache License, Version 2.0.

// StorageRegistry — tracks which IDataEngine backend schemes are
// available in this build.
//
// Each backend registers its scheme name at static-init time via
// RegisterBackend.  CLI validators (e.g. ValidateBucketUrl) consult
// Available() to reject unsupported schemes before any backend is
// instantiated.
//
// Instantiation itself still goes through DataEngineFactory, which
// dispatches on the scheme directly — StorageRegistry intentionally
// doesn't carry a Factory, so backends stay free to construct
// themselves however they like.

#pragma once

#include <string>
#include <string_view>
#include <unordered_set>

namespace swordfs::storage {

/// Registry of available IDataEngine backend schemes.
class StorageRegistry {
 public:
  static StorageRegistry &Instance();

  /// Register a backend scheme.  Called at static-init time.
  void Register(std::string_view name);

  /// Return true if a backend with the given scheme is registered.
  bool Available(std::string_view name) const;

 private:
  StorageRegistry() = default;
  std::unordered_set<std::string> schemes_;
};

/// RAII helper: registers a backend scheme at static-init time.
struct RegisterBackend {
  RegisterBackend(std::string_view name) {
    StorageRegistry::Instance().Register(name);
  }
};

}  // namespace swordfs::storage