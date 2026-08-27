// Copyright 2026 SwordFS Contributors.
// Licensed under the Apache License, Version 2.0.

#include "metadata/types/Entry.hpp"

#include "metadata/types/BufCodec.hpp"

namespace swordfs::metadata {

utils::Status SwordFsEntry::SerializeTo(std::string *out) const {
  if (out == nullptr || name.empty() || ino == 0) {
    return utils::Status::InvalidArgument("Invalid directory entry record");
  }
  BufEncoder enc;
  enc.Header(RecordType::kEntry);
  enc.String(name);
  enc.U32(type);
  enc.U64(ino);
  enc.Finish(out);
  return utils::Status::OK();
}

utils::Status SwordFsEntry::ParseFrom(std::string_view data) {
  BufDecoder dec(data);
  dec.Header(RecordType::kEntry);
  dec.String(&name);
  dec.U32(&type);
  dec.U64(&ino);
  if (!dec || name.empty() || ino == 0 || !dec.Done()) {
    return utils::Status::Malformed("Malformed directory entry record");
  }
  return utils::Status::OK();
}

}  // namespace swordfs::metadata
