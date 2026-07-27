// Copyright 2026 SwordFS Contributors.
// Licensed under the Apache License, Version 2.0.

#include <gtest/gtest.h>

#include <memory>

#include "metadata/IMetaEngine.hpp"
#include "metadata/MetaEngineFactory.hpp"

using swordfs::metadata::CreateMetaEngine;
using swordfs::metadata::IMetaEngine;
using swordfs::utils::Status;

TEST(MetaEngineFactoryTest, CreateMemoryEngine) {
  std::unique_ptr<IMetaEngine> engine;
  Status st = CreateMetaEngine("memory://local", &engine);
  EXPECT_TRUE(st.ok()) << st.message();
  EXPECT_NE(engine, nullptr);
}

TEST(MetaEngineFactoryTest, CreateUnsupportedEngine) {
  std::unique_ptr<IMetaEngine> engine;
  Status st = CreateMetaEngine("redis://localhost:6379/0", &engine);
  EXPECT_FALSE(st.ok());
  EXPECT_EQ(engine, nullptr);
}

TEST(MetaEngineFactoryTest, CreateEmptyUrl) {
  std::unique_ptr<IMetaEngine> engine;
  Status st = CreateMetaEngine("", &engine);
  // Empty URL doesn't match memory:// → unsupported
  EXPECT_FALSE(st.ok());
}
