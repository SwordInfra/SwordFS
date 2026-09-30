// Copyright 2026 SwordFS Contributors.
// Licensed under the Apache License, Version 2.0.

#include "metadata/PosixAcl.hpp"

#include <sys/stat.h>

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <string>
#include <utility>
#include <vector>

namespace swordfs::metadata {

namespace {

constexpr uint32_t kAclXattrVersion = 2;
constexpr uint16_t kAclUserObj = 0x01;
constexpr uint16_t kAclUser = 0x02;
constexpr uint16_t kAclGroupObj = 0x04;
constexpr uint16_t kAclGroup = 0x08;
constexpr uint16_t kAclMask = 0x10;
constexpr uint16_t kAclOther = 0x20;
constexpr uint16_t kAclPermMask = 0x07;
constexpr uint32_t kAclUndefinedId = std::numeric_limits<uint32_t>::max();
constexpr size_t kAclHeaderSize = 4;
constexpr size_t kAclEntrySize = 8;

struct AclEntry {
  uint16_t tag = 0;
  uint16_t perm = 0;
  uint32_t id = kAclUndefinedId;
};

uint16_t ReadLe16(std::string_view data, size_t offset) {
  return static_cast<uint16_t>(static_cast<uint8_t>(data[offset])) |
         (static_cast<uint16_t>(static_cast<uint8_t>(data[offset + 1])) << 8);
}

uint32_t ReadLe32(std::string_view data, size_t offset) {
  return static_cast<uint32_t>(static_cast<uint8_t>(data[offset])) |
         (static_cast<uint32_t>(static_cast<uint8_t>(data[offset + 1])) << 8) |
         (static_cast<uint32_t>(static_cast<uint8_t>(data[offset + 2])) << 16) |
         (static_cast<uint32_t>(static_cast<uint8_t>(data[offset + 3])) << 24);
}

void AppendLe16(std::string *out, uint16_t value) {
  out->push_back(static_cast<char>(value & 0xff));
  out->push_back(static_cast<char>((value >> 8) & 0xff));
}

void AppendLe32(std::string *out, uint32_t value) {
  out->push_back(static_cast<char>(value & 0xff));
  out->push_back(static_cast<char>((value >> 8) & 0xff));
  out->push_back(static_cast<char>((value >> 16) & 0xff));
  out->push_back(static_cast<char>((value >> 24) & 0xff));
}

class PosixAcl {
 public:
  static utils::Status Parse(std::string_view data, PosixAcl &out) {
    if (data.size() < kAclHeaderSize || (data.size() - kAclHeaderSize) % kAclEntrySize != 0) {
      return utils::Status::InvalidArgument("malformed POSIX ACL xattr size");
    }
    if (ReadLe32(data, 0) != kAclXattrVersion) {
      return utils::Status::InvalidArgument("unsupported POSIX ACL xattr version");
    }

    PosixAcl parsed;
    const size_t count = (data.size() - kAclHeaderSize) / kAclEntrySize;
    parsed.entries_.reserve(count);
    for (size_t i = 0; i < count; ++i) {
      const size_t offset = kAclHeaderSize + i * kAclEntrySize;
      parsed.entries_.push_back({
          .tag = ReadLe16(data, offset),
          .perm = ReadLe16(data, offset + 2),
          .id = ReadLe32(data, offset + 4),
      });
    }
    auto status = parsed.Validate();
    if (!status.ok()) {
      return status;
    }
    out = std::move(parsed);
    return utils::Status::OK();
  }

  std::string Serialize() const {
    std::string value;
    value.reserve(kAclHeaderSize + entries_.size() * kAclEntrySize);
    AppendLe32(&value, kAclXattrVersion);
    for (const auto &entry : entries_) {
      AppendLe16(&value, entry.tag);
      AppendLe16(&value, entry.perm);
      AppendLe32(&value, entry.id);
    }
    return value;
  }

  bool IsMinimal() const {
    return entries_.size() == 3 && entries_[0].tag == kAclUserObj && entries_[1].tag == kAclGroupObj &&
           entries_[2].tag == kAclOther;
  }

  uint32_t PermissionsMode() const {
    const AclEntry *user_obj = Find(kAclUserObj);
    const AclEntry *group_class = Find(kAclMask);
    if (group_class == nullptr) {
      group_class = Find(kAclGroupObj);
    }
    const AclEntry *other = Find(kAclOther);
    return (static_cast<uint32_t>(user_obj->perm) << 6) | (static_cast<uint32_t>(group_class->perm) << 3) |
           static_cast<uint32_t>(other->perm);
  }

  void ApplyMode(uint32_t mode) {
    FindMutable(kAclUserObj)->perm = static_cast<uint16_t>((mode >> 6) & kAclPermMask);
    AclEntry *group_class = FindMutable(kAclMask);
    if (group_class == nullptr) {
      group_class = FindMutable(kAclGroupObj);
    }
    group_class->perm = static_cast<uint16_t>((mode >> 3) & kAclPermMask);
    FindMutable(kAclOther)->perm = static_cast<uint16_t>(mode & kAclPermMask);
  }

  void RestrictToMode(uint32_t mode) {
    FindMutable(kAclUserObj)->perm &= static_cast<uint16_t>((mode >> 6) & kAclPermMask);
    AclEntry *group_class = FindMutable(kAclMask);
    if (group_class == nullptr) {
      group_class = FindMutable(kAclGroupObj);
    }
    group_class->perm &= static_cast<uint16_t>((mode >> 3) & kAclPermMask);
    FindMutable(kAclOther)->perm &= static_cast<uint16_t>(mode & kAclPermMask);
  }

 private:
  utils::Status Validate() const {
    if (entries_.size() < 3) {
      return utils::Status::InvalidArgument("POSIX ACL is missing required entries");
    }

    enum class State : uint8_t {
      kUserObj,
      kUsers,
      kGroupObj,
      kGroups,
      kMask,
      kOther,
      kDone,
    };
    State state = State::kUserObj;
    uint32_t last_user_id = 0;
    uint32_t last_group_id = 0;
    bool have_user_id = false;
    bool have_group_id = false;
    bool has_named = false;
    bool has_mask = false;

    for (const auto &entry : entries_) {
      if ((entry.perm & ~kAclPermMask) != 0) {
        return utils::Status::InvalidArgument("POSIX ACL entry has invalid permission bits");
      }
      const bool named = entry.tag == kAclUser || entry.tag == kAclGroup;
      if (named) {
        if (entry.id == kAclUndefinedId) {
          return utils::Status::InvalidArgument("named POSIX ACL entry has undefined id");
        }
      } else if (entry.id != kAclUndefinedId) {
        return utils::Status::InvalidArgument("POSIX ACL object entry has a concrete id");
      }

      switch (entry.tag) {
        case kAclUserObj:
          if (state != State::kUserObj) {
            return utils::Status::InvalidArgument("POSIX ACL user_obj entry is out of order");
          }
          state = State::kUsers;
          break;
        case kAclUser:
          if (state != State::kUsers) {
            return utils::Status::InvalidArgument("POSIX ACL user entry is out of order");
          }
          if (have_user_id && entry.id <= last_user_id) {
            return utils::Status::InvalidArgument("POSIX ACL named users are not strictly ordered");
          }
          last_user_id = entry.id;
          have_user_id = true;
          has_named = true;
          break;
        case kAclGroupObj:
          if (state != State::kUsers) {
            return utils::Status::InvalidArgument("POSIX ACL group_obj entry is out of order");
          }
          state = State::kGroups;
          break;
        case kAclGroup:
          if (state != State::kGroups) {
            return utils::Status::InvalidArgument("POSIX ACL group entry is out of order");
          }
          if (have_group_id && entry.id <= last_group_id) {
            return utils::Status::InvalidArgument("POSIX ACL named groups are not strictly ordered");
          }
          last_group_id = entry.id;
          have_group_id = true;
          has_named = true;
          break;
        case kAclMask:
          if (state != State::kGroups) {
            return utils::Status::InvalidArgument("POSIX ACL mask entry is out of order");
          }
          has_mask = true;
          state = State::kOther;
          break;
        case kAclOther:
          if (state != State::kGroups && state != State::kOther) {
            return utils::Status::InvalidArgument("POSIX ACL other entry is out of order");
          }
          state = State::kDone;
          break;
        default:
          return utils::Status::InvalidArgument("POSIX ACL entry has unsupported tag");
      }
    }
    if (state != State::kDone) {
      return utils::Status::InvalidArgument("POSIX ACL is missing required owner/group/other entries");
    }
    if (has_named && !has_mask) {
      return utils::Status::InvalidArgument("extended POSIX ACL is missing mask entry");
    }
    return utils::Status::OK();
  }

  const AclEntry *Find(uint16_t tag) const {
    auto it = std::find_if(entries_.begin(), entries_.end(), [tag](const AclEntry &entry) { return entry.tag == tag; });
    return it == entries_.end() ? nullptr : &*it;
  }

  AclEntry *FindMutable(uint16_t tag) {
    auto it = std::find_if(entries_.begin(), entries_.end(), [tag](const AclEntry &entry) { return entry.tag == tag; });
    return it == entries_.end() ? nullptr : &*it;
  }

 private:
  std::vector<AclEntry> entries_;
};

utils::Status CheckSetMode(const SwordFsInode &inode, std::string_view name, XAttrSetMode mode) {
  const bool exists = inode.xattrs.find(std::string(name)) != inode.xattrs.end();
  switch (mode) {
    case XAttrSetMode::kUpsert:
      return utils::Status::OK();
    case XAttrSetMode::kCreateOnly:
      return exists ? utils::Status::AlreadyExists("xattr already exists") : utils::Status::OK();
    case XAttrSetMode::kReplaceOnly:
      return exists ? utils::Status::OK() : utils::Status::NoData("xattr not found");
  }
  return utils::Status::InvalidArgument("invalid xattr set mode");
}

utils::Status CheckInsertListSize(const SwordFsInode &inode, std::string_view name) {
  if (inode.xattrs.find(std::string(name)) != inode.xattrs.end()) {
    return utils::Status::OK();
  }
  size_t list_bytes = name.size() + 1;
  for (const auto &[existing_name, existing_value] : inode.xattrs) {
    (void)existing_value;
    list_bytes += existing_name.size() + 1;
  }
  if (list_bytes > kMaxXAttrListSize) {
    return utils::Status::Range("xattr name list exceeds maximum size");
  }
  return utils::Status::OK();
}

utils::Status ParseStoredAcl(std::string_view value, PosixAcl &acl) {
  auto status = PosixAcl::Parse(value, acl);
  if (!status.ok()) {
    return utils::Status::Malformed("stored POSIX ACL is malformed");
  }
  return utils::Status::OK();
}

}  // namespace

bool IsPosixAclXAttrName(std::string_view name) {
  return name == kPosixAclAccessXAttr || name == kPosixAclDefaultXAttr;
}

utils::Status SetPosixAclXAttr(SwordFsInode *inode, std::string_view name, std::string_view value, XAttrSetMode mode) {
  if (inode == nullptr) {
    return utils::Status::InvalidArgument("inode is null");
  }
  if (!IsPosixAclXAttrName(name)) {
    return utils::Status::InvalidArgument("not a POSIX ACL xattr");
  }
  if (inode->IsSymlink()) {
    return utils::Status::OperationNotSupported("POSIX ACLs are not supported on symlinks");
  }
  if (name == kPosixAclDefaultXAttr && !inode->IsDir()) {
    return utils::Status::Permission("default POSIX ACL requires a directory");
  }
  if (value.size() > kMaxXAttrValueSize) {
    return utils::Status::Range("xattr value exceeds maximum size");
  }
  auto status = CheckSetMode(*inode, name, mode);
  if (!status.ok()) {
    return status;
  }

  PosixAcl acl;
  status = PosixAcl::Parse(value, acl);
  if (!status.ok()) {
    return status;
  }

  if ((name == kPosixAclDefaultXAttr || !acl.IsMinimal())) {
    status = CheckInsertListSize(*inode, name);
    if (!status.ok()) {
      return status;
    }
  }

  if (name == kPosixAclAccessXAttr) {
    inode->attr.mode = (inode->attr.mode & ~0777u) | acl.PermissionsMode();
    if (acl.IsMinimal()) {
      inode->xattrs.erase(std::string(name));
    } else {
      inode->xattrs[std::string(name)] = acl.Serialize();
    }
  } else {
    inode->xattrs[std::string(name)] = acl.Serialize();
  }
  inode->Touch(SetAttrField::kCtime);
  return utils::Status::OK();
}

utils::Status RemovePosixAclXAttr(SwordFsInode *inode, std::string_view name) {
  if (inode == nullptr) {
    return utils::Status::InvalidArgument("inode is null");
  }
  if (!IsPosixAclXAttrName(name)) {
    return utils::Status::InvalidArgument("not a POSIX ACL xattr");
  }
  if (inode->IsSymlink()) {
    return utils::Status::OperationNotSupported("POSIX ACLs are not supported on symlinks");
  }
  if (name == kPosixAclDefaultXAttr && !inode->IsDir()) {
    return utils::Status::Permission("default POSIX ACL requires a directory");
  }
  auto it = inode->xattrs.find(std::string(name));
  if (it == inode->xattrs.end()) {
    return utils::Status::NoData("POSIX ACL xattr not found");
  }
  inode->xattrs.erase(it);
  inode->Touch(SetAttrField::kCtime);
  return utils::Status::OK();
}

utils::Status SyncPosixAccessAclForMode(SwordFsInode *inode, uint32_t mode) {
  if (inode == nullptr) {
    return utils::Status::InvalidArgument("inode is null");
  }
  auto it = inode->xattrs.find(std::string(kPosixAclAccessXAttr));
  if (it == inode->xattrs.end()) {
    return utils::Status::OK();
  }
  PosixAcl acl;
  auto status = ParseStoredAcl(it->second, acl);
  if (!status.ok()) {
    return status;
  }
  acl.ApplyMode(mode);
  inode->xattrs[std::string(kPosixAclAccessXAttr)] = acl.Serialize();
  return utils::Status::OK();
}

utils::Status ApplyPosixAclCreateInheritance(const SwordFsInode &parent, uint32_t umask, SwordFsInode *child) {
  if (child == nullptr) {
    return utils::Status::InvalidArgument("child inode is null");
  }
  if (child->IsSymlink()) {
    return utils::Status::OK();
  }

  auto default_it = parent.xattrs.find(std::string(kPosixAclDefaultXAttr));
  if (default_it == parent.xattrs.end()) {
    child->attr.mode = (child->attr.mode & ~0777u) | ((child->attr.mode & 0777u) & ~umask);
    return utils::Status::OK();
  }

  PosixAcl access_acl;
  auto status = PosixAcl::Parse(default_it->second, access_acl);
  if (!status.ok()) {
    return utils::Status::Malformed("parent default POSIX ACL is malformed");
  }
  access_acl.RestrictToMode(child->attr.mode);
  child->attr.mode = (child->attr.mode & ~0777u) | access_acl.PermissionsMode();
  if (!access_acl.IsMinimal()) {
    child->xattrs[std::string(kPosixAclAccessXAttr)] = access_acl.Serialize();
  }
  if (child->IsDir()) {
    child->xattrs[std::string(kPosixAclDefaultXAttr)] = default_it->second;
  }
  return utils::Status::OK();
}

}  // namespace swordfs::metadata
