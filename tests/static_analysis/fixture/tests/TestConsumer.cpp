#include "Production.hpp"

namespace fixture::test {

void ExerciseTestOnlySurface() {
  TestOnlyFunction();
  Overloaded(std::string_view{"test"});
  TestAlias value = 0;
  (void)value;
}

}  // namespace fixture::test
