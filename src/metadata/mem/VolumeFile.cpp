// Copyright 2026 SwordFS Contributors.
// Licensed under the Apache License, Version 2.0.

#include "metadata/mem/VolumeFile.hpp"

#include <folly/FileUtil.h>
#include <folly/logging/xlog.h>
#include <folly/portability/Filesystem.h>

#include <string>
#include <system_error>

#include "utils/Logging.hpp"

namespace swordfs::metadata::mem {
namespace {
constexpr std::string_view kConfigRoot = "/etc/swordfs";
constexpr std::string_view kConfigFileName = "/volume.fmt";

std::string FilePath(std::string_view volume_name) {
  return std::string(kConfigRoot) + "/" + std::string(volume_name) +
         std::string(kConfigFileName);
}
}  // namespace

utils::Status VolumeFile::Write(const SwordFsVolume &volume) {
  std::error_code ec;
  const std::string directory = std::string(kConfigRoot) + "/" + volume_name_;
  if (!folly::fs::exists(kConfigRoot, ec)) {
    if (ec) {
      return utils::Status::IOError("failed to access config root: " + std::string(kConfigRoot) + ": " + ec.message());
    }
    folly::fs::create_directories(kConfigRoot, ec);
    if (ec) {
      return utils::Status::IOError("failed to create config root: " + std::string(kConfigRoot) + ": " + ec.message());
    }
  }
  if (!folly::fs::exists(directory, ec)) {
    if (ec) {
      return utils::Status::IOError("failed to access volume directory: " + directory + ": " + ec.message());
    }
    folly::fs::create_directories(directory, ec);
    if (ec) {
      return utils::Status::IOError("failed to create volume directory: " + directory + ": " + ec.message());
    }
  } else if (!folly::fs::is_directory(directory, ec) || ec) {
    return utils::Status::InvalidArgument("volume path is not a directory: " + directory);
  }

  const std::string file_path = FilePath(volume_name_);
  if (!folly::writeFile(volume.SerializeTo(), file_path.c_str())) {
    return utils::Status::IOError("failed to write " + file_path);
  }
  SWORDFS_LOG_INFO << "Volume config written to " << file_path;
  return utils::Status::OK();
}

utils::Status VolumeFile::Read(SwordFsVolume *volume) {
  if (volume == nullptr) {
    return utils::Status::InvalidArgument("volume output is null");
  }

  const std::string file_path = FilePath(volume_name_);
  std::string content;
  if (!folly::readFile(file_path.c_str(), content)) {
    return utils::Status::NotFound("volume config not found at " + file_path);
  }
  return volume->ParseFrom(content);
}

bool VolumeFile::Exists() const {
  const std::string file_path = FilePath(volume_name_);
  std::error_code ec;
  return folly::fs::exists(file_path, ec) && !ec;
}

}  // namespace swordfs::metadata::mem
