// Copyright 2026 SwordFS Contributors.
// Licensed under the Apache License, Version 2.0.

#include "volume/VolumeConfig.hpp"

#include <folly/FileUtil.h>
#include <folly/json.h>
#include <folly/portability/Filesystem.h>

#include "config/ConfigCenter.hpp"
#include "utils/Logging.hpp"

namespace swordfs::volume {

std::string VolumeConfig::ToJson() const {
  folly::dynamic root = folly::dynamic::object;
  root["name"] = name;
  root["meta"] = meta_url;
  root["storage"] = storage;
  root["bucket"] = bucket;
  root["region"] = region;

  return folly::toPrettyJson(root);
}

Status VolumeConfig::FromJson(std::string_view json) {
  folly::dynamic root;
  try {
    root = folly::parseJson(json);
  } catch (const std::exception& e) {
    return Status::InvalidArgument(
        std::string("invalid JSON in volume.json: ") + e.what());
  }

  if (!root.isObject())
    return Status::InvalidArgument("volume.json root is not an object");

  if (!root.count("name") || !root["name"].isString())
    return Status::InvalidArgument("missing or invalid 'name' in volume.json");
  name = root["name"].asString();

  if (!root.count("meta") || !root["meta"].isString())
    return Status::InvalidArgument("missing or invalid 'meta' in volume.json");
  meta_url = root["meta"].asString();

  if (!root.count("storage") || !root["storage"].isString())
    return Status::InvalidArgument("missing or invalid 'storage' in volume.json");
  storage = root["storage"].asString();

  if (!root.count("bucket") || !root["bucket"].isString())
    return Status::InvalidArgument("missing or invalid 'bucket' in volume.json");
  bucket = root["bucket"].asString();

  if (!root.count("region") || !root["region"].isString())
    return Status::InvalidArgument("missing or invalid 'region' in volume.json");
  region = root["region"].asString();

  return Status::OK();
}

Status VolumeConfig::WriteToFile(const std::string& path) const {
  std::error_code ec;
  if (!folly::fs::exists(path, ec)) {
    if (ec) {
      return Status::IOError("failed to access volume directory: " +
                             path + ": " + ec.message());
    }
    folly::fs::create_directories(path, ec);
    if (ec) {
      return Status::IOError("failed to create volume directory: " +
                             path + ": " + ec.message());
    }
  } else if (!folly::fs::is_directory(path, ec) || ec) {
    return Status::InvalidArgument("volume path is not a directory: " +
                                   path);
  }

  std::string file_path = path + "/volume.json";
  std::string json = ToJson();
  if (!folly::writeFile(json, file_path.c_str())) {
    return Status::IOError("failed to write " + file_path);
  }

  SWORDFS_LOG_INFO << "Volume config written to " << file_path;
  return Status::OK();
}

Status VolumeConfig::ReadFromFile(const std::string& path) {
  std::string file_path = path + "/volume.json";
  std::string content;
  if (!folly::readFile(file_path.c_str(), content)) {
    return Status::NotFound("volume.json not found at " + file_path);
  }

  return FromJson(content);
}

std::string VolumeConfig::MountHint() const {
  const std::string& config_path =
      swordfs::config::ConfigCenter::Instance().volume_config_path();
  std::string hint = "swordfs mount --volume " + name;
  hint += " --meta " + meta_url;
  if (!config_path.empty()) {
    hint += " --volume-config-path " + config_path;
  }
  hint += " /mnt/swordfs";
  return hint;
}

}  // namespace swordfs::volume
