// Copyright 2026 SwordFS Contributors.
// Licensed under the Apache License, Version 2.0.

#include "storage/VolumeConfig.hpp"

#include <folly/portability/Filesystem.h>

#include <fstream>
#include <random>
#include <sstream>

#include "utils/Logging.hpp"

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

namespace {

std::string JsonEscape(std::string_view s) {
  std::string out;
  out.reserve(s.size() + 2);
  for (char c : s) {
    switch (c) {
      case '"':  out += "\\\""; break;
      case '\\': out += "\\\\"; break;
      case '\n': out += "\\n"; break;
      case '\r': out += "\\r"; break;
      case '\t': out += "\\t"; break;
      default:   out += c; break;
    }
  }
  return out;
}

std::string JsonUnescape(std::string_view s) {
  std::string out;
  out.reserve(s.size());
  for (size_t i = 0; i < s.size(); ++i) {
    if (s[i] == '\\' && i + 1 < s.size()) {
      switch (s[++i]) {
        case '"':  out += '"';  break;
        case '\\': out += '\\'; break;
        case 'n':  out += '\n'; break;
        case 'r':  out += '\r'; break;
        case 't':  out += '\t'; break;
        default:   out += '\\'; out += s[i]; break;
      }
    } else {
      out += s[i];
    }
  }
  return out;
}

bool ExtractString(std::string_view json, std::string_view key,
                   std::string* value) {
  std::string search = "\"" + std::string(key) + "\"";
  auto pos = json.find(search);
  if (pos == std::string_view::npos) return false;

  pos = json.find(':', pos + search.size());
  if (pos == std::string_view::npos) return false;
  ++pos;

  while (pos < json.size() && (json[pos] == ' ' || json[pos] == '\n' ||
                                json[pos] == '\r' || json[pos] == '\t'))
    ++pos;

  if (pos >= json.size() || json[pos] != '"') return false;
  ++pos;

  auto close = json.find('"', pos);
  while (close != std::string_view::npos && close > 0 && json[close - 1] == '\\')
    close = json.find('"', close + 1);
  if (close == std::string_view::npos) return false;

  *value = JsonUnescape(json.substr(pos, close - pos));
  return true;
}

}  // anonymous namespace

std::string VolumeConfig::ToJson() const {
  std::ostringstream os;
  os << "{\n";
  os << "  \"uuid\": \"" << JsonEscape(uuid) << "\",\n";
  os << "  \"storage\": \"" << JsonEscape(storage) << "\",\n";
  os << "  \"s3_config\": {\n";
  os << "    \"bucket\": \"" << JsonEscape(s3_config.bucket) << "\",\n";
  os << "    \"endpoint\": \"" << JsonEscape(s3_config.endpoint) << "\",\n";
  os << "    \"region\": \"" << JsonEscape(s3_config.region) << "\",\n";
  os << "    \"prefix\": \"" << JsonEscape(s3_config.prefix) << "\"\n";
  os << "  }\n";
  os << "}\n";
  return os.str();
}

utils::Status VolumeConfig::FromJson(std::string_view json, VolumeConfig* out) {
  if (!ExtractString(json, "uuid", &out->uuid))
    return utils::Status::InvalidArgument("missing uuid in volume.json");
  if (!ExtractString(json, "storage", &out->storage))
    return utils::Status::InvalidArgument("missing storage in volume.json");
  ExtractString(json, "bucket", &out->s3_config.bucket);
  ExtractString(json, "endpoint", &out->s3_config.endpoint);
  ExtractString(json, "region", &out->s3_config.region);
  ExtractString(json, "prefix", &out->s3_config.prefix);
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

}  // namespace swordfs::storage
