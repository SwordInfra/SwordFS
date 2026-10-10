// Copyright 2026 SwordFS Contributors.
// Licensed under the Apache License, Version 2.0.

#pragma once

#include "metadata/IMetaEngine.hpp"

namespace swordfs::test {

// Default-failing interface adapter for isolated upper-layer tests. Unlike a
// memory filesystem it implements no namespace, persistence or transactions.
// A derived fixture overrides only the exact methods the component consumes.
class UnsupportedMetaEngine : public metadata::IMetaEngine {
 public:
  utils::Status Initialize() override {
    return utils::Status::OK();
  }
  utils::Status FormatVolume(const metadata::SwordFsVolume &) override {
    return utils::Status::OK();
  }
  utils::Status LoadVolume(metadata::SwordFsVolume *) override {
    return utils::Status::OK();
  }
  metadata::Limits GetLimits() const override {
    return {255, 0};
  }

  utils::Status Lookup(metadata::InodeID, std::string_view, metadata::SwordFsInode *) override {
    return Unsupported();
  }
  utils::Status GetInode(metadata::InodeID, metadata::SwordFsInode *) override {
    return Unsupported();
  }
  utils::Status GetInodes(const std::vector<metadata::InodeID> &,
                          std::vector<std::optional<metadata::SwordFsInode>> *) override {
    return Unsupported();
  }
  utils::Status Create(metadata::InodeID, std::string_view, uint32_t, metadata::SwordFsInode *) override {
    return Unsupported();
  }
  utils::Status MkNod(metadata::InodeID, std::string_view, uint32_t, uint64_t, metadata::SwordFsInode *) override {
    return Unsupported();
  }
  utils::Status MkDir(metadata::InodeID, std::string_view, uint32_t, metadata::SwordFsInode *) override {
    return Unsupported();
  }
  utils::Status Unlink(metadata::InodeID, std::string_view, std::optional<metadata::InodeID>) override {
    return Unsupported();
  }
  utils::Status RmDir(metadata::InodeID, std::string_view) override {
    return Unsupported();
  }
  utils::Status Rename(metadata::InodeID, std::string_view, metadata::InodeID, std::string_view,
                       metadata::RenameFlag) override {
    return Unsupported();
  }
  utils::Status SetAttr(metadata::InodeID, const metadata::SwordFsAttr &, metadata::SetAttrField,
                        metadata::SwordFsInode *) override {
    return Unsupported();
  }
  utils::Status SetInodeFlags(metadata::InodeID, metadata::InodeFlag, metadata::SwordFsInode *) override {
    return Unsupported();
  }
  utils::Status SetXAttr(metadata::InodeID, std::string_view, std::string_view, metadata::XAttrSetMode) override {
    return Unsupported();
  }
  utils::Status GetXAttr(metadata::InodeID, std::string_view, std::string *) override {
    return Unsupported();
  }
  utils::Status ListXAttrs(metadata::InodeID, std::vector<std::string> *) override {
    return Unsupported();
  }
  utils::Status RemoveXAttr(metadata::InodeID, std::string_view) override {
    return Unsupported();
  }
  utils::Status StatFs(metadata::SwordFsStatFs *) override {
    return Unsupported();
  }
  utils::Status Symlink(metadata::InodeID, std::string_view, std::string_view, metadata::SwordFsInode *) override {
    return Unsupported();
  }
  utils::Status Link(metadata::InodeID, metadata::InodeID, std::string_view, metadata::SwordFsInode *) override {
    return Unsupported();
  }
  utils::Status Readlink(metadata::InodeID, std::string *) override {
    return Unsupported();
  }
  utils::Status Open(metadata::InodeID, uint64_t *, metadata::InodeFlag *) override {
    return Unsupported();
  }
  utils::Status PrepareReclaim(metadata::InodeID) override {
    return Unsupported();
  }
  utils::Status CompleteReclaim(metadata::InodeID) override {
    return Unsupported();
  }
  utils::Status VisitOrphanCandidates(const metadata::InodeVisitorFn &) override {
    return Unsupported();
  }
  utils::Status VisitPendingReclaims(const metadata::ReclaimVisitorFn &) override {
    return Unsupported();
  }
  utils::Status VisitPendingDeletesBatch(size_t, const metadata::PendingDeleteVisitorFn &, bool *) override {
    return Unsupported();
  }
  utils::Status CompletePendingDelete(std::string_view) override {
    return Unsupported();
  }
  utils::Status AllocateChunkRevision(metadata::ChunkRevision *) override {
    return Unsupported();
  }
  utils::Status OpenDir(metadata::InodeID, metadata::DirIteratorPtr *) override {
    return Unsupported();
  }
  utils::Status CommitChunk(metadata::InodeID, const std::optional<metadata::SwordFsChunk> &,
                            const metadata::SwordFsChunk &) override {
    return Unsupported();
  }
  utils::Status FindChunk(metadata::InodeID, metadata::ChunkIndex, metadata::SwordFsChunk *) override {
    return Unsupported();
  }
  utils::Status Truncate(metadata::InodeID, uint64_t) override {
    return Unsupported();
  }

 private:
  static utils::Status Unsupported() {
    return utils::Status::NotSupported("not exercised by this test double");
  }
};

}  // namespace swordfs::test
