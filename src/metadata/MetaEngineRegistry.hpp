// Copyright 2026 SwordFS Contributors.
// Licensed under the Apache License, Version 2.0.

#pragma once

#include <string>
#include <string_view>
#include <vector>

namespace swordfs::metadata {

/// Describes a metadata engine accepted by the metadata URL configuration.
struct MetaEngineDescriptor {
  std::string name;
  std::string url_syntax;
};

/// Registry of metadata engines available in this build.
class MetaEngineRegistry {
 public:
  static MetaEngineRegistry& Instance();

  /// Register a metadata engine. Called at static initialization time.
  void Register(std::string_view name, std::string_view url_syntax);

  /// Return the engines registered in this build.
  const std::vector<MetaEngineDescriptor>& Engines() const;

  /// Return a human-readable list of supported metadata URL syntaxes.
  std::string SupportedUrlSyntaxes() const;

 private:
  MetaEngineRegistry() = default;
  std::vector<MetaEngineDescriptor> engines_;
};

/// RAII helper for registering a metadata engine at static initialization.
struct RegisterMetaEngine {
  RegisterMetaEngine(std::string_view name, std::string_view url_syntax) {
    MetaEngineRegistry::Instance().Register(name, url_syntax);
  }
};

}  // namespace swordfs::metadata
