// Copyright 2026 SwordFS Contributors.
// Licensed under the Apache License, Version 2.0.

#include "storage/DataEngineRegistry.hpp"

#include "storage/IDataEngine.hpp"

namespace swordfs::storage {

DataEngineRegistry &DataEngineRegistry::Instance() {
  static DataEngineRegistry registry;
  return registry;
}

void DataEngineRegistry::Register(std::string_view name, Factory factory) {
  factories_.emplace(std::string(name), factory);
}

bool DataEngineRegistry::Available(std::string_view name) const {
  return factories_.find(std::string(name)) != factories_.end();
}

utils::Status DataEngineRegistry::Create(std::string_view name, std::unique_ptr<IDataEngine> *out) const {
  if (out == nullptr) {
    return utils::Status::InvalidArgument("data engine output is null");
  }
  const auto it = factories_.find(std::string(name));
  if (it == factories_.end()) {
    return utils::Status::NotSupported("unknown data storage scheme: " + std::string(name));
  }
  return it->second(out);
}

}  // namespace swordfs::storage
