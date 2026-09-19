#pragma once

#include <cstdint>
#include <mutex>
#include <string_view>

namespace fixture {

using DeadAlias = std::uint64_t;
using TestAlias = std::uint64_t;
using Callback = void (*)();

void ProductionUsed();
void DeadFunction();
void TestOnlyFunction();

void Overloaded(int value);
void Overloaded(std::string_view value);

class Interface {
 public:
  virtual ~Interface() = default;
  virtual void Run() = 0;
};

class Implementation final : public Interface {
 public:
  void Run() override {
  }
};

class Lockable {
 public:
  void lock() {
  }
  void unlock() {
  }
};

struct Comparable {
  int value;
};

bool operator==(const Comparable &lhs, const Comparable &rhs);

void FrameworkCallback();
void RegisteredFactory();
void ProductionConsumer();

}  // namespace fixture
