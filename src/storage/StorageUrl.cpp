// Copyright 2026 SwordFS Contributors.
// Licensed under the Apache License, Version 2.0.

#include "storage/StorageUrl.hpp"

#include <cctype>

namespace swordfs::utils {

bool StorageUrl::Parse(std::string_view url, StorageUrl *out) {
  if (!out) {
    return false;
  }

  // Find scheme
  auto scheme_end = url.find("://");
  if (scheme_end == std::string_view::npos) {
    return false;
  }

  out->scheme = url.substr(0, scheme_end);
  for (char &c : out->scheme) {
    c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
  }
  auto rest = url.substr(scheme_end + 3);

  // Find path separator
  auto path_start = rest.find('/');
  if (path_start == std::string_view::npos) {
    out->host = rest;
    out->path.clear();
  } else {
    out->host = rest.substr(0, path_start);
    out->path = rest.substr(path_start);
  }

  // Treat trailing "/" as empty path for readability
  if (out->path == "/") {
    out->path.clear();
  }

  return true;
}

}  // namespace swordfs::utils
