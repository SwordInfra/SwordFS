// Copyright 2026 SwordFS Contributors.
// Licensed under the Apache License, Version 2.0.

#include <gtest/gtest.h>

#include <cerrno>

#include "utils/Status.hpp"

using swordfs::utils::Status;

TEST(StatusTest, Ok) {
  Status st = Status::OK();
  EXPECT_TRUE(st.ok());
  EXPECT_EQ(st.code(), Status::kOk);
  EXPECT_EQ(st.ToErrno(), 0);
}

TEST(StatusTest, NotFound) {
  Status st = Status::NotFound("missing");
  EXPECT_FALSE(st.ok());
  EXPECT_TRUE(st.IsNotFound());
  EXPECT_EQ(st.code(), Status::kNotFound);
  EXPECT_EQ(st.ToErrno(), ENOENT);
}

TEST(StatusTest, AlreadyExists) {
  Status st = Status::AlreadyExists("dup");
  EXPECT_TRUE(st.IsAlreadyExists());
  EXPECT_EQ(st.ToErrno(), EEXIST);
}

TEST(StatusTest, NotDirectory) {
  Status st = Status::NotDirectory("bad");
  EXPECT_TRUE(st.IsNotDirectory());
  EXPECT_EQ(st.ToErrno(), ENOTDIR);
}

TEST(StatusTest, InvalidArgument) {
  Status st = Status::InvalidArgument("bad arg");
  EXPECT_EQ(st.code(), Status::kInvalidArgument);
  EXPECT_EQ(st.ToErrno(), EINVAL);
}

TEST(StatusTest, NotSupported) {
  Status st = Status::NotSupported("unsupported");
  EXPECT_TRUE(st.IsNotSupported());
  EXPECT_EQ(st.ToErrno(), ENOSYS);
}

TEST(StatusTest, IOError) {
  Status st = Status::IOError("io error");
  EXPECT_EQ(st.ToErrno(), EIO);
}

TEST(StatusTest, Busy) {
  Status st = Status::Busy("busy");
  EXPECT_TRUE(st.IsBusy());
  EXPECT_EQ(st.ToErrno(), EBUSY);
}

TEST(StatusTest, NoSpace) {
  Status st = Status::NoSpace("full");
  EXPECT_EQ(st.ToErrno(), ENOSPC);
}

TEST(StatusTest, Permission) {
  Status st = Status::Permission("denied");
  EXPECT_TRUE(st.IsPermission());
  EXPECT_EQ(st.ToErrno(), EACCES);
}

TEST(StatusTest, NoMemory) {
  Status st = Status::NoMemory("oom");
  EXPECT_EQ(st.ToErrno(), ENOMEM);
}

TEST(StatusTest, Internal) {
  Status st = Status::Internal("internal");
  EXPECT_EQ(st.ToErrno(), EIO);
}

TEST(StatusTest, Message) {
  Status st = Status::NotFound("file not found: foo.txt");
  EXPECT_EQ(st.message(), "file not found: foo.txt");
}
