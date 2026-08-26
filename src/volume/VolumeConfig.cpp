// Copyright 2026 SwordFS Contributors.
// Licensed under the Apache License, Version 2.0.

#include "volume/VolumeConfig.hpp"

#include <folly/FileUtil.h>
#include <folly/io/Cursor.h>
#include <folly/io/IOBuf.h>
#include <folly/json.h>
#include <folly/portability/Filesystem.h>

#include <cstdint>
#include <limits>
#include <memory>

#include "config/ConfigCenter.hpp"
#include <folly/logging/xlog.h>
#include "utils/Logging.hpp"

namespace swordfs::volume {

namespace {
constexpr const char* kConfigFileName = "/volume.json";
constexpr std::string_view kVolumeMagic = "SWORVOL1";
constexpr uint32_t kVolumeSchemaVersion = 1;

class Writer {
 public:
  Writer()
      : buffer_(folly::IOBuf::create(256)), appender_(buffer_.get(), 256) {}

  void U32(uint32_t value) { appender_.writeLE<uint32_t>(value); }
  void U64(uint64_t value) { appender_.writeLE<uint64_t>(value); }
  void String(std::string_view value) {
    U64(value.size());
    appender_.push(reinterpret_cast<const uint8_t *>(value.data()), value.size());
  }
  void Finish(std::string *out) { buffer_->appendTo(*out); }

 private:
  std::unique_ptr<folly::IOBuf> buffer_;
  folly::io::Appender appender_;
};

class Reader {
 public:
  explicit Reader(std::string_view data)
      : buffer_(folly::IOBuf::wrapBuffer(data.data(), data.size())),
        cursor_(buffer_.get()) {}

  bool U32(uint32_t *value) { return cursor_.tryReadLE(*value); }
  bool U64(uint64_t *value) { return cursor_.tryReadLE(*value); }
  bool String(std::string *value) {
    uint64_t length = 0;
    if (!U64(&length) || length > 16 * 1024 * 1024 || !cursor_.canAdvance(length)) {
      return false;
    }
    *value = cursor_.readFixedString(length);
    return true;
  }
  bool Header() {
    std::string magic;
    uint32_t version = 0;
    return String(&magic) && magic == kVolumeMagic && U32(&version) &&
           version == kVolumeSchemaVersion;
  }
  bool Done() const { return cursor_.isAtEnd(); }

 private:
  std::unique_ptr<folly::IOBuf> buffer_;
  folly::io::Cursor cursor_;
};
}

std::string VolumeConfig::SerializeTo() const {
  std::string out;
  Writer writer;
  writer.String(kVolumeMagic);
  writer.U32(kVolumeSchemaVersion);
  writer.String(name);
  writer.String(meta_url);
  writer.String(storage);
  writer.String(bucket);
  writer.String(region);
  writer.U64(chunk_size);
  writer.Finish(&out);
  return out;
}

Status VolumeConfig::ParseFrom(std::string_view data) {
  Reader reader(data);
  VolumeConfig config;
  uint64_t chunk_size_value = 0;
  if (!reader.Header() || !reader.String(&config.name) ||
      !reader.String(&config.meta_url) || !reader.String(&config.storage) ||
      !reader.String(&config.bucket) || !reader.String(&config.region) ||
      !reader.U64(&chunk_size_value) || chunk_size_value == 0 ||
      chunk_size_value > std::numeric_limits<size_t>::max() || !reader.Done()) {
    return Status::Malformed("Malformed volume metadata record");
  }
  config.chunk_size = static_cast<size_t>(chunk_size_value);
  *this = std::move(config);
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

  std::string file_path = path + kConfigFileName;
  std::string json = ToJson();
  if (!folly::writeFile(json, file_path.c_str())) {
    return Status::IOError("failed to write " + file_path);
  }

  SWORDFS_LOG_INFO << "Volume config written to " << file_path;
  return Status::OK();
}

Status VolumeConfig::ReadFromFile(const std::string& path) {
  std::string file_path = path + kConfigFileName;
  std::string content;
  if (!folly::readFile(file_path.c_str(), content)) {
    return Status::NotFound("volume.json not found at " + file_path);
  }

  return FromJson(content);
}

std::string VolumeConfig::ToJson() const {
  folly::dynamic root = folly::dynamic::object;
  root["name"] = name;
  root["meta"] = meta_url;
  root["storage"] = storage;
  root["bucket"] = bucket;
  root["region"] = region;
  root["chunk_size"] = static_cast<int64_t>(chunk_size);
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
  if (!root.isObject()) {
    return Status::InvalidArgument("volume.json root is not an object");
  }

  VolumeConfig config;
  if (!root.count("name") || !root["name"].isString()) {
    return Status::InvalidArgument("missing or invalid 'name' in volume.json");
  }
  if (!root.count("meta") || !root["meta"].isString()) {
    return Status::InvalidArgument("missing or invalid 'meta' in volume.json");
  }
  if (!root.count("storage") || !root["storage"].isString()) {
    return Status::InvalidArgument("missing or invalid 'storage' in volume.json");
  }
  if (!root.count("bucket") || !root["bucket"].isString()) {
    return Status::InvalidArgument("missing or invalid 'bucket' in volume.json");
  }
  if (!root.count("region") || !root["region"].isString()) {
    return Status::InvalidArgument("missing or invalid 'region' in volume.json");
  }
  config.name = root["name"].asString();
  config.meta_url = root["meta"].asString();
  config.storage = root["storage"].asString();
  config.bucket = root["bucket"].asString();
  config.region = root["region"].asString();
  if (root.count("chunk_size")) {
    if (!root["chunk_size"].isInt() || root["chunk_size"].asInt() <= 0) {
      return Status::InvalidArgument("invalid 'chunk_size' in volume.json");
    }
    config.chunk_size = static_cast<size_t>(root["chunk_size"].asInt());
  }
  *this = std::move(config);
  return Status::OK();
}

bool VolumeConfig::ConfigFileExists(const std::string& path) {
  std::string file_path = path + kConfigFileName;
  std::error_code ec;
  return folly::fs::exists(file_path, ec) && !ec;
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
