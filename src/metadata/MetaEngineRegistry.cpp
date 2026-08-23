// Copyright 2026 SwordFS Contributors.
// Licensed under the Apache License, Version 2.0.

#include "metadata/MetaEngineRegistry.hpp"

#include <folly/String.h>

namespace swordfs::metadata {

MetaEngineRegistry& MetaEngineRegistry::Instance() {
  static MetaEngineRegistry registry;
  return registry;
}

void MetaEngineRegistry::Register(std::string_view name,
                                  std::string_view url_syntax) {
  engines_.push_back({std::string(name), std::string(url_syntax)});
}

const std::vector<MetaEngineDescriptor>& MetaEngineRegistry::Engines() const {
  return engines_;
}

std::string MetaEngineRegistry::SupportedUrlSyntaxes() const {
  std::vector<std::string> syntaxes;
  syntaxes.reserve(engines_.size());
  for (const auto& engine : engines_) {
    syntaxes.push_back(engine.url_syntax);
  }
  return folly::join(", ", syntaxes);
}

}  // namespace swordfs::metadata
