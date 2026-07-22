// Copyright 2026 SwordFS Contributors.
// Licensed under the Apache License, Version 2.0.

#include "meta/KeyBuilder.hpp"

#include <cstring>

namespace swordfs::meta {

// ── Internal helpers ──────────────────────────────────────────────────────

void KeyBuilder::AppendUint64(uint64_t v) {
  buf_.push_back(static_cast<char>((v >> 56) & 0xFF));
  buf_.push_back(static_cast<char>((v >> 48) & 0xFF));
  buf_.push_back(static_cast<char>((v >> 40) & 0xFF));
  buf_.push_back(static_cast<char>((v >> 32) & 0xFF));
  buf_.push_back(static_cast<char>((v >> 24) & 0xFF));
  buf_.push_back(static_cast<char>((v >> 16) & 0xFF));
  buf_.push_back(static_cast<char>((v >> 8) & 0xFF));
  buf_.push_back(static_cast<char>(v & 0xFF));
}

void KeyBuilder::AppendUint32(uint32_t v) {
  buf_.push_back(static_cast<char>((v >> 24) & 0xFF));
  buf_.push_back(static_cast<char>((v >> 16) & 0xFF));
  buf_.push_back(static_cast<char>((v >> 8) & 0xFF));
  buf_.push_back(static_cast<char>(v & 0xFF));
}

uint64_t KeyBuilder::ReadUint64(std::string_view data, size_t offset) {
  uint64_t v = 0;
  for (size_t i = 0; i < 8; ++i) {
    v = (v << 8) | static_cast<uint8_t>(data[offset + i]);
  }
  return v;
}

uint32_t KeyBuilder::ReadUint32(std::string_view data, size_t offset) {
  uint32_t v = 0;
  for (size_t i = 0; i < 4; ++i) {
    v = (v << 8) | static_cast<uint8_t>(data[offset + i]);
  }
  return v;
}

// ── Factory methods ───────────────────────────────────────────────────────

KeyBuilder KeyBuilder::Attr(uint64_t inode) {
  KeyBuilder kb;
  kb.buf_.push_back(static_cast<char>(KeyPrefix::kAttr));
  kb.AppendUint64(inode);
  return kb;
}

KeyBuilder KeyBuilder::Counter(uint64_t slice_id, uint32_t size) {
  KeyBuilder kb;
  kb.buf_.push_back(static_cast<char>(KeyPrefix::kCounter));
  kb.AppendUint64(slice_id);
  kb.AppendUint32(size);
  return kb;
}

KeyBuilder KeyBuilder::Session(uint64_t session_id) {
  KeyBuilder kb;
  kb.buf_.push_back(static_cast<char>(KeyPrefix::kSession));
  kb.AppendUint64(session_id);
  return kb;
}

// ── Chaining methods ──────────────────────────────────────────────────────

KeyBuilder& KeyBuilder::Inode() {
  buf_.push_back(static_cast<char>(AttrSuffix::kInode));
  return *this;
}

KeyBuilder& KeyBuilder::Entry(std::string_view name) {
  buf_.push_back(static_cast<char>(AttrSuffix::kEntry));
  buf_.append(name);
  return *this;
}

KeyBuilder& KeyBuilder::Parent(uint64_t parent_inode) {
  buf_.push_back(static_cast<char>(AttrSuffix::kParent));
  AppendUint64(parent_inode);
  return *this;
}

KeyBuilder& KeyBuilder::Chunk(uint32_t chunk_index) {
  buf_.push_back(static_cast<char>(AttrSuffix::kChunk));
  AppendUint32(chunk_index);
  return *this;
}

KeyBuilder& KeyBuilder::Symlink() {
  buf_.push_back(static_cast<char>(AttrSuffix::kSymlink));
  return *this;
}

KeyBuilder& KeyBuilder::Xattr(std::string_view name) {
  buf_.push_back(static_cast<char>(AttrSuffix::kXattr));
  buf_.append(name);
  return *this;
}

// ── Static convenience builders ───────────────────────────────────────────

std::string KeyBuilder::InodeKey(uint64_t inode) {
  return Attr(inode).Inode().Build();
}

std::string KeyBuilder::EntryKey(uint64_t parent, std::string_view name) {
  return Attr(parent).Entry(name).Build();
}

std::string KeyBuilder::EntryScanPrefix(uint64_t parent) {
  // A{parent}D — the prefix for all entries under this directory.
  return Attr(parent).Build() + static_cast<char>(AttrSuffix::kEntry);
}

std::string KeyBuilder::SliceKey(uint64_t slice_id, uint32_t size) {
  return Counter(slice_id, size).Build();
}

std::string KeyBuilder::ChunkKey(uint64_t inode, uint32_t chunk_index) {
  return Attr(inode).Chunk(chunk_index).Build();
}

std::string KeyBuilder::NextInodeKey() {
  // "nextInode" counter — stored as a special key outside the normal prefix
  // space. We use a dedicated counter key pattern.
  std::string key;
  key.push_back(static_cast<char>(KeyPrefix::kCounter));
  key.append("nextInode");
  return key;
}

std::string KeyBuilder::NextSliceKey() {
  std::string key;
  key.push_back(static_cast<char>(KeyPrefix::kCounter));
  key.append("nextSlice");
  return key;
}

std::string KeyBuilder::NextSessionKey() {
  std::string key;
  key.push_back(static_cast<char>(KeyPrefix::kCounter));
  key.append("nextSession");
  return key;
}

std::string KeyBuilder::SymlinkKey(uint64_t inode) {
  return Attr(inode).Symlink().Build();
}

std::string KeyBuilder::XattrKey(uint64_t inode, std::string_view name) {
  return Attr(inode).Xattr(name).Build();
}

std::string KeyBuilder::ParentKey(uint64_t inode, uint64_t parent) {
  return Attr(inode).Parent(parent).Build();
}

// ── Parsing ───────────────────────────────────────────────────────────────

bool KeyBuilder::ParseInodeKey(std::string_view key, uint64_t* inode) {
  // Expected: A{inode:8}I = 10 bytes
  if (key.size() != 10) return false;
  if (static_cast<uint8_t>(key[0]) != static_cast<uint8_t>(KeyPrefix::kAttr))
    return false;
  if (static_cast<uint8_t>(key[9]) != static_cast<uint8_t>(AttrSuffix::kInode))
    return false;
  *inode = ReadUint64(key, 1);
  return true;
}

bool KeyBuilder::ParseEntryKey(std::string_view key, uint64_t* parent,
                               std::string_view* name) {
  // Expected: A{parent:8}D{name} — minimum 10 bytes (1 + 8 + 1 + 0)
  if (key.size() < 10) return false;
  if (static_cast<uint8_t>(key[0]) != static_cast<uint8_t>(KeyPrefix::kAttr))
    return false;
  if (static_cast<uint8_t>(key[9]) != static_cast<uint8_t>(AttrSuffix::kEntry))
    return false;
  *parent = ReadUint64(key, 1);
  *name = key.substr(10);
  return true;
}

}  // namespace swordfs::meta
