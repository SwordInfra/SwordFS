// Copyright 2026 SwordFS Contributors.
// Licensed under the Apache License, Version 2.0.

#include "metadata/MetaEngineFactory.hpp"

#include "metadata/MetaEngineRegistry.hpp"
#include "metadata/Utils.hpp"

namespace swordfs::metadata {

utils::Status CreateMetaEngine(std::string_view meta_url, std::unique_ptr<IMetaEngine> *out) {
  if (out == nullptr) {
    return utils::Status::InvalidArgument("metadata engine output is null");
  }

  std::string scheme;
  auto status = ParseUrlScheme(meta_url, &scheme);
  if (!status.ok()) {
    return status;
  }
  return MetaEngineRegistry::Instance().Create(scheme, meta_url, out);
}

}  // namespace swordfs::metadata
