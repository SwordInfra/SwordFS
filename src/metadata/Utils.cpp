// Copyright 2026 SwordFS Contributors.
// Licensed under the Apache License, Version 2.0.

#include "metadata/Utils.hpp"

#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>

#include <cctype>
#include <cstring>

#include "dirent.h"
#include "metadata/Types.hpp"

namespace swordfs::metadata {

struct stat MakeStat(mode_t mode, time_t now) {
  struct stat st;
  std::memset(&st, 0, sizeof(st));
  st.st_mode = mode;
  st.st_nlink = S_ISDIR(mode) ? 2 : 1;
  st.st_size = S_ISDIR(mode) ? 4096 : 0;
  st.st_uid = ::getuid();
  st.st_gid = ::getgid();
  st.st_blksize = 4096;
  st.st_atime = now;
  st.st_mtime = now;
  st.st_ctime = now;
  return st;
}

// Convert st_mode to dirent type (DT_DIR, DT_REG, etc.)
uint32_t ModeToDt(mode_t mode) {
  if (S_ISDIR(mode)) return DT_DIR;
  if (S_ISREG(mode)) return DT_REG;
  if (S_ISLNK(mode)) return DT_LNK;
  if (S_ISBLK(mode)) return DT_BLK;
  if (S_ISCHR(mode)) return DT_CHR;
  if (S_ISFIFO(mode)) return DT_FIFO;
  if (S_ISSOCK(mode)) return DT_SOCK;
  return DT_UNKNOWN;
}

void KillSUID(struct stat *st) {
  if (st->st_mode & S_ISUID) {
    st->st_mode &= ~S_ISUID;
  }
  if (st->st_mode & S_ISGID) {
    st->st_mode &= ~S_ISGID;
  }
}

utils::Status ParseUrlScheme(std::string_view url, std::string *scheme) {
  if (scheme == nullptr) {
    return utils::Status::InvalidArgument("URL scheme output is null");
  }

  const auto pos = url.find("://");
  if (pos == std::string_view::npos || pos == 0) {
    return utils::Status::InvalidArgument(
        "URL must have a scheme:// prefix: " + std::string(url));
  }

  scheme->assign(url.substr(0, pos));
  for (char &c : *scheme) {
    c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
  }
  return utils::Status::OK();
}

}  // namespace swordfs::metadata
