// Copyright 2026 SwordFS Contributors.
// Licensed under the Apache License, Version 2.0.

// StorageUrl — lightweight URL parser for storage engine configuration.
//
// Used by --meta and --bucket to express backend type and address
// in a single parameter, e.g.:
//
//   memory://local
//   redis://127.0.0.1:6379/0
//   s3://mybucket.s3.amazonaws.com/chunks

#pragma once

#include <string>
#include <string_view>

namespace swordfs::utils {

struct StorageUrl {
  std::string scheme;  // e.g. "memory", "redis", "s3"
  std::string host;    // e.g. "local", "127.0.0.1:6379",
                       //       "mybucket.s3.amazonaws.com"
  std::string path;    // e.g. "/0", "/chunks"

  /// Parse a URL of the form scheme://host/path.  Returns true on success.
  static bool Parse(std::string_view url, StorageUrl* out);

  /// Reconstruct the URL string.
  std::string ToString() const;

  bool empty() const { return scheme.empty(); }
};

}  // namespace swordfs::utils
