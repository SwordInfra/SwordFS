// Copyright 2026 SwordFS Contributors.
// Licensed under the Apache License, Version 2.0.

#include "storage/VolumeConfig.hpp"
#include "storage/IDataEngine.hpp"

#include <folly/json.h>
#include <folly/portability/Filesystem.h>

#include <fstream>
#include <random>
#include <sstream>

#include "utils/Logging.hpp"

#ifdef SWORDFS_ENABLE_S3
#include "storage/s3/S3DataEngine.hpp"
#endif

namespace swordfs::storage {

std::string VolumeConfig::GenerateUUID() {
  static std::random_device rd;
  static std::mt19937_64 gen(rd());
  static std::uniform_int_distribution<uint64_t> dis;

  uint64_t a = dis(gen);
  uint64_t b = dis(gen);

  char buf[37];
  snprintf(buf, sizeof(buf),
           "%08x-%04x-4%03x-%04lx-%012lx",
           static_cast<uint32_t>(a),
           static_cast<uint16_t>(a >> 32),
           static_cast<uint16_t>(b & 0xFFF),
           static_cast<unsigned long>((b >> 12) & 0x3FFF) | 0x8000UL,
           static_cast<unsigned long>(b >> 28));
  return buf;
}

std::string VolumeConfig::ToJson() const {
  folly::dynamic s3_cfg = folly::dynamic::object;
  s3_cfg["bucket"] = s3_config.bucket;
  s3_cfg["endpoint"] = s3_config.endpoint;
  s3_cfg["region"] = s3_config.region;
  s3_cfg["prefix"] = s3_config.prefix;

  folly::dynamic root = folly::dynamic::object;
  root["uuid"] = uuid;
  root["storage"] = storage;
  root["s3_config"] = std::move(s3_cfg);

  return folly::toPrettyJson(root);
}

utils::Status VolumeConfig::FromJson(std::string_view json, VolumeConfig* out) {
  folly::dynamic root;
  try {
    root = folly::parseJson(json);
  } catch (const std::exception& e) {
    return utils::Status::InvalidArgument(
        std::string("invalid JSON in volume.json: ") + e.what());
  }

  if (!root.isObject())
    return utils::Status::InvalidArgument("volume.json root is not an object");

  if (!root.count("uuid") || !root["uuid"].isString())
    return utils::Status::InvalidArgument("missing uuid in volume.json");
  out->uuid = root["uuid"].asString();

  if (root.count("storage") && root["storage"].isString())
    out->storage = root["storage"].asString();

  if (root.count("s3_config") && root["s3_config"].isObject()) {
    auto& s3 = root["s3_config"];
    if (s3.count("bucket") && s3["bucket"].isString())
      out->s3_config.bucket = s3["bucket"].asString();
    if (s3.count("endpoint") && s3["endpoint"].isString())
      out->s3_config.endpoint = s3["endpoint"].asString();
    if (s3.count("region") && s3["region"].isString())
      out->s3_config.region = s3["region"].asString();
    if (s3.count("prefix") && s3["prefix"].isString())
      out->s3_config.prefix = s3["prefix"].asString();
  }

  return utils::Status::OK();
}

utils::Status VolumeConfig::WriteToFile(const std::string& path) const {
  std::error_code ec;
  if (!folly::fs::exists(path)) {
    folly::fs::create_directories(path, ec);
    if (ec) {
      return utils::Status::IOError("failed to create volume directory: " +
                                     path + ": " + ec.message());
    }
  } else if (!folly::fs::is_directory(path)) {
    return utils::Status::InvalidArgument("volume path is not a directory: " +
                                           path);
  }

  std::string file_path = path + "/volume.json";
  std::ofstream ofs(file_path);
  if (!ofs) {
    return utils::Status::IOError("failed to open " + file_path +
                                   " for writing");
  }
  ofs << ToJson();
  if (!ofs) {
    return utils::Status::IOError("failed to write " + file_path);
  }

  SWORDFS_LOG_INFO << "Volume config written to " << file_path;
  return utils::Status::OK();
}

utils::Status VolumeConfig::ReadFromFile(const std::string& path,
                                          VolumeConfig* out) {
  std::string file_path = path + "/volume.json";
  std::ifstream ifs(file_path);
  if (!ifs) {
    return utils::Status::NotFound("volume.json not found at " + file_path);
  }

  std::ostringstream oss;
  oss << ifs.rdbuf();
  if (!ifs) {
    return utils::Status::IOError("failed to read " + file_path);
  }

  return FromJson(oss.str(), out);
}

std::unique_ptr<IDataEngine> CreateDataEngine(const VolumeConfig& vol) {
  if (vol.storage == "s3") {
#ifdef SWORDFS_ENABLE_S3
    S3Config s3_cfg;
    s3_cfg.bucket = vol.s3_config.bucket;
    s3_cfg.endpoint = vol.s3_config.endpoint;
    s3_cfg.region = vol.s3_config.region;
    s3_cfg.prefix = vol.s3_config.prefix;
    return std::make_unique<S3DataEngine>(s3_cfg);
#else
    return nullptr;
#endif
  }
  return nullptr;
}

}  // namespace swordfs::storage
