// Copyright 2026 SwordFS Contributors.
// Licensed under the Apache License, Version 2.0.

#include "metadata/mem/VolumeJson.hpp"

#include <folly/FileUtil.h>
#include <folly/json.h>
#include <folly/logging/xlog.h>
#include <folly/portability/Filesystem.h>

#include <exception>
#include <system_error>
#include <utility>

#include "utils/Logging.hpp"

namespace swordfs::metadata::mem {
namespace {
constexpr const char *kConfigFileName = "/volume.json";
}

std::string VolumeJson::Serialize(const VolumeFormat &volume) {
  folly::dynamic root = folly::dynamic::object;
  root["name"] = volume.name;
  root["meta"] = volume.meta_url;
  root["storage"] = volume.storage;
  root["bucket"] = volume.bucket;
  root["region"] = volume.region;
  root["chunk_size"] = static_cast<int64_t>(volume.chunk_size);
  return folly::toPrettyJson(root);
}

utils::Status VolumeJson::Parse(std::string_view json, VolumeFormat *volume) {
  if (volume == nullptr) {
    return utils::Status::InvalidArgument("volume output is null");
  }

  folly::dynamic root;
  try {
    root = folly::parseJson(json);
  } catch (const std::exception &e) {
    return utils::Status::InvalidArgument(std::string("invalid JSON in volume.json: ") + e.what());
  }
  if (!root.isObject()) {
    return utils::Status::InvalidArgument("volume.json root is not an object");
  }

  VolumeFormat parsed;
  if (!root.count("name") || !root["name"].isString()) {
    return utils::Status::InvalidArgument("missing or invalid 'name' in volume.json");
  }
  if (!root.count("meta") || !root["meta"].isString()) {
    return utils::Status::InvalidArgument("missing or invalid 'meta' in volume.json");
  }
  if (!root.count("storage") || !root["storage"].isString()) {
    return utils::Status::InvalidArgument("missing or invalid 'storage' in volume.json");
  }
  if (!root.count("bucket") || !root["bucket"].isString()) {
    return utils::Status::InvalidArgument("missing or invalid 'bucket' in volume.json");
  }
  if (!root.count("region") || !root["region"].isString()) {
    return utils::Status::InvalidArgument("missing or invalid 'region' in volume.json");
  }

  parsed.name = root["name"].asString();
  parsed.meta_url = root["meta"].asString();
  parsed.storage = root["storage"].asString();
  parsed.bucket = root["bucket"].asString();
  parsed.region = root["region"].asString();
  if (root.count("chunk_size")) {
    if (!root["chunk_size"].isInt() || root["chunk_size"].asInt() <= 0) {
      return utils::Status::InvalidArgument("invalid 'chunk_size' in volume.json");
    }
    parsed.chunk_size = static_cast<size_t>(root["chunk_size"].asInt());
  }
  *volume = std::move(parsed);
  return utils::Status::OK();
}

utils::Status VolumeJson::Write(const VolumeFormat &volume, const std::string &path) {
  std::error_code ec;
  if (!folly::fs::exists(path, ec)) {
    if (ec) {
      return utils::Status::IOError("failed to access volume directory: " + path + ": " + ec.message());
    }
    folly::fs::create_directories(path, ec);
    if (ec) {
      return utils::Status::IOError("failed to create volume directory: " + path + ": " + ec.message());
    }
  } else if (!folly::fs::is_directory(path, ec) || ec) {
    return utils::Status::InvalidArgument("volume path is not a directory: " + path);
  }

  const std::string file_path = path + kConfigFileName;
  if (!folly::writeFile(Serialize(volume), file_path.c_str())) {
    return utils::Status::IOError("failed to write " + file_path);
  }
  SWORDFS_LOG_INFO << "Volume config written to " << file_path;
  return utils::Status::OK();
}

utils::Status VolumeJson::Read(const std::string &path, VolumeFormat *volume) {
  const std::string file_path = path + kConfigFileName;
  std::string content;
  if (!folly::readFile(file_path.c_str(), content)) {
    return utils::Status::NotFound("volume.json not found at " + file_path);
  }
  return Parse(content, volume);
}

bool VolumeJson::Exists(const std::string &path) {
  const std::string file_path = path + kConfigFileName;
  std::error_code ec;
  return folly::fs::exists(file_path, ec) && !ec;
}

}  // namespace swordfs::metadata::mem
