#include "test_framework.hpp"

namespace testing {

std::vector<TestCase>& registry() {
  static std::vector<TestCase> r;
  return r;
}
int& failureCount() {
  static int n = 0;
  return n;
}

}  // namespace testing

int main() {
  int ran = 0;
  for (auto& t : testing::registry()) {
    std::cout << "[ RUN  ] " << t.name << "\n";
    try {
      t.fn();
      std::cout << "[  OK  ] " << t.name << "\n";
    } catch (const testing::CheckFailure&) {
      std::cout << "[ FAIL ] " << t.name << "\n";
    } catch (const std::exception& e) {
      std::cerr << "    unexpected exception: " << e.what() << "\n";
      testing::failureCount()++;
      std::cout << "[ FAIL ] " << t.name << "\n";
    }
    ran++;
  }
  std::cout << ran << " tests, " << testing::failureCount() << " failures\n";
  return testing::failureCount() == 0 ? 0 : 1;
}
