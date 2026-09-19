#include "Production.hpp"

namespace fixture {

void ProductionUsed() {
}

void DeadFunction() {
}

void TestOnlyFunction() {
}

void Overloaded(int) {
}

void Overloaded(std::string_view) {
}

void FrameworkCallback() {
}

void RegisteredFactory() {
}

bool operator==(const Comparable &lhs, const Comparable &rhs) {
  return lhs.value == rhs.value;
}

namespace {

class Registrar {
 public:
  explicit Registrar(Callback callback) : callback_(callback) {
  }

 private:
  Callback callback_;
};

Registrar kRegistrar{&RegisteredFactory};

}  // namespace

void ProductionConsumer() {
  ProductionUsed();
  Overloaded(1);

  Callback callback = &FrameworkCallback;
  callback();

  Implementation implementation;
  Interface *interface = &implementation;
  interface->Run();

  Lockable lockable;
  std::lock_guard<Lockable> lock(lockable);
}

}  // namespace fixture
