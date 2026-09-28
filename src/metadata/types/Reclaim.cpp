// Copyright 2026 SwordFS Contributors.
// Licensed under the Apache License, Version 2.0.

#include "metadata/types/Reclaim.hpp"

#include <utility>

#include "metadata/types/BufCodec.hpp"

namespace swordfs::metadata {

utils::Status ReclaimWork::SerializeTo(std::string *out) const {
  if (out == nullptr || ino == 0 || payload.empty()) {
    return utils::Status::InvalidArgument("Invalid opaque reclaim work");
  }
  BufEncoder enc;
  enc.Header(RecordType::kReclaim);
  enc.U64(ino);
  enc.String(payload);
  enc.Finish(out);
  return utils::Status::OK();
}

utils::Status ReclaimWork::ParseFrom(std::string_view data) {
  BufDecoder dec(data);
  ReclaimWork parsed;
  dec.Header(RecordType::kReclaim);
  dec.U64(&parsed.ino);
  dec.String(&parsed.payload);
  if (!dec || !dec.Done() || parsed.ino == 0 || parsed.payload.empty()) {
    return utils::Status::Malformed("Malformed opaque reclaim work");
  }
  *this = std::move(parsed);
  return utils::Status::OK();
}

utils::Status PendingDelete::SerializeTo(std::string *out) const {
  if (out == nullptr || id.empty() || payload.empty()) {
    return utils::Status::InvalidArgument("Invalid opaque pending delete");
  }
  BufEncoder enc;
  enc.Header(RecordType::kPendingDelete);
  enc.String(id);
  enc.String(payload);
  enc.Finish(out);
  return utils::Status::OK();
}

utils::Status PendingDelete::ParseFrom(std::string_view data) {
  BufDecoder dec(data);
  PendingDelete parsed;
  dec.Header(RecordType::kPendingDelete);
  dec.String(&parsed.id);
  dec.String(&parsed.payload);
  if (!dec || !dec.Done() || parsed.id.empty() || parsed.payload.empty()) {
    return utils::Status::Malformed("Malformed opaque pending delete");
  }
  *this = std::move(parsed);
  return utils::Status::OK();
}

}  // namespace swordfs::metadata
