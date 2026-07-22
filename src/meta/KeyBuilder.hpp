// Copyright 2026 SwordFS Contributors.
// Licensed under the Apache License, Version 2.0.

// KeyBuilder — construct and parse metadata engine keys using JuiceFS-compatible
// prefix-based encoding.
//
// All file system metadata is stored as flat key-value pairs. Keys use a
// single-byte type tag followed by fixed-width binary-encoded fields, ensuring
// lexicographic ordering matches semantic ordering:
//
//   A{inode:8bytes big-endian}I         → inode attributes
//   A{parent:8bytes big-endian}D{name}  → directory entry
//   K{slice_id:8bytes}{size:4bytes}     → slice reference count
//
// This allows Readdir to be a simple prefix scan over A{parent}D*.

#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>

namespace swordfs::meta {

// Fixed field sizes (matching JuiceFS).
inline constexpr size_t kInodeSize = 8;      // uint64 big-endian
inline constexpr size_t kSliceIdSize = 8;    // uint64 big-endian
inline constexpr size_t kChunkIndexSize = 4; // uint32 big-endian
inline constexpr size_t kSessionIdSize = 8;  // uint64 big-endian

// Key prefix bytes (single-byte type tags).
enum class KeyPrefix : uint8_t {
  kAttr = 'A',       // Attributes and entries
  kCounter = 'K',    // Slice reference counts
  kSession = 'S',    // Session info
  kDirStat = 'U',    // Directory statistics
  kFileLock = 'F',   // POSIX file locks
  kPLock = 'P',      // POSIX file locks (plural)
  kDeleted = 'D',    // Deleted inodes
  kDetached = 'N',   // Detached nodes
  kQuota = 'Q',      // Directory quotas
};

// Key suffix bytes (single-byte type tags within the A-prefix namespace).
enum class AttrSuffix : uint8_t {
  kInode = 'I',      // A{inode}I → inode attributes
  kEntry = 'D',      // A{parent}D{name} → directory entry → child inode
  kParent = 'P',     // A{inode}P{parent} → hardlink parent tracking
  kChunk = 'C',      // A{inode}C{index:4bytes} → chunk slice list
  kSymlink = 'S',    // A{inode}S → symlink target
  kXattr = 'X',      // A{inode}X{name} → extended attribute
};

/// Fluent builder for metadata keys.
///
/// Example:
///   auto key = KeyBuilder::Attr(ino).Inode().Build();
///   auto entry = KeyBuilder::Entry(parent, "filename").Build();
class KeyBuilder {
 public:
  KeyBuilder() = default;

  // ── Factory methods ───────────────────────────────────────────────────

  /// Start an attribute-space key (prefix 'A').
  static KeyBuilder Attr(uint64_t inode);

  /// Start a counter-space key (prefix 'K').
  static KeyBuilder Counter(uint64_t slice_id, uint32_t size);

  /// Start a session-space key (prefix 'S').
  static KeyBuilder Session(uint64_t session_id);

  // ── Chaining methods for A-prefix keys ────────────────────────────────

  /// Append the inode attribute suffix 'I'.
  KeyBuilder& Inode();

  /// Append the directory entry suffix 'D' + entry name.
  KeyBuilder& Entry(std::string_view name);

  /// Append the parent tracking suffix 'P' + parent inode.
  KeyBuilder& Parent(uint64_t parent_inode);

  /// Append the chunk index suffix 'C' + chunk index.
  KeyBuilder& Chunk(uint32_t chunk_index);

  /// Append the symlink suffix 'S'.
  KeyBuilder& Symlink();

  /// Append the xattr suffix 'X' + attribute name.
  KeyBuilder& Xattr(std::string_view name);

  // ── Build / access ────────────────────────────────────────────────────

  /// Return the constructed key.
  std::string Build() const { return buf_; }

  /// Return a view of the constructed key.
  std::string_view View() const { return buf_; }

  // ── Static helpers for common key patterns ────────────────────────────

  /// Build A{inode}I (inode attributes).
  static std::string InodeKey(uint64_t inode);

  /// Build A{parent}D{name} (directory entry start, for scan prefix).
  static std::string EntryKey(uint64_t parent, std::string_view name);

  /// Build A{parent}D prefix for scanning all entries under a directory.
  static std::string EntryScanPrefix(uint64_t parent);

  /// Build K{slice_id}{size} (slice reference count).
  static std::string SliceKey(uint64_t slice_id, uint32_t size);

  /// Build A{inode}C{index} (chunk slice list).
  static std::string ChunkKey(uint64_t inode, uint32_t chunk_index);

  /// Build the next-inode counter key.
  static std::string NextInodeKey();

  /// Build the next-slice counter key.
  static std::string NextSliceKey();

  /// Build the next-session counter key.
  static std::string NextSessionKey();

  /// Build A{inode}S (symlink target).
  static std::string SymlinkKey(uint64_t inode);

  /// Build A{inode}X{name} (extended attribute).
  static std::string XattrKey(uint64_t inode, std::string_view name);

  /// Build A{inode}P{parent} (hardlink parent).
  static std::string ParentKey(uint64_t inode, uint64_t parent);

  // ── Parsing ───────────────────────────────────────────────────────────

  /// Parse an inode key to extract the inode number.
  static bool ParseInodeKey(std::string_view key, uint64_t* inode);

  /// Parse an entry key to extract parent inode and name.
  static bool ParseEntryKey(std::string_view key, uint64_t* parent,
                            std::string_view* name);

 private:
  /// Append a uint64 in big-endian encoding.
  void AppendUint64(uint64_t v);

  /// Append a uint32 in big-endian encoding.
  void AppendUint32(uint32_t v);

  /// Read a uint64 from big-endian encoding at offset.
  static uint64_t ReadUint64(std::string_view data, size_t offset);

  /// Read a uint32 from big-endian encoding at offset.
  static uint32_t ReadUint32(std::string_view data, size_t offset);

  std::string buf_;
};

}  // namespace swordfs::meta
