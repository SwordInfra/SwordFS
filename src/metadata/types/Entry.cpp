// Copyright 2026 SwordFS Contributors.
// Licensed under the Apache License, Version 2.0.

#include "metadata/types/Entry.hpp"

#include "metadata/types/BufCodec.hpp"

namespace swordfs::metadata {

utils::Status SwordFsEntry::SerializeTo(std::string *out) const {
  if (out == nullptr || name.empty() || ino == 0) {
    return utils::Status::InvalidArgument("Invalid directory entry record");
  }
  BufEncoder writer;
  writer.Header(RecordType::kEntry);
  writer.String(name);
  writer.U32(type);
  writer.U64(ino);
  writer.Finish(out);
  return utils::Status::OK();
}

utils::Status SwordFsEntry::ParseFrom(std::string_view data, SwordFsEntry *out) {
  if (out == nullptr) {
    return utils::Status::InvalidArgument("Directory entry output is null");
  }
  BufDecoder reader(data);
  SwordFsEntry entry;
  if (!reader.Header(RecordType::kEntry) || !reader.String(&entry.name) || entry.name.empty() ||
      !reader.U32(&entry.type) || !reader.U64(&entry.ino) || entry.ino == 0 || !reader.Done()) {
    return utils::Status::Malformed("Malformed directory entry record");
  }
  *out = std::move(entry);
  return utils::Status::OK();
}

}  // namespace swordfs::metadata
