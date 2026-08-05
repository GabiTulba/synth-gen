#pragma once
#include <cmath>
#include <functional>
#include <iostream>
#include <sstream>
#include <string>
#include <vector>

namespace testing {

struct TestCase {
  std::string name;
  std::function<void()> fn;
};

std::vector<TestCase>& registry();
int& failureCount();

struct Registrar {
  Registrar(std::string name, std::function<void()> fn) {
    registry().push_back({std::move(name), std::move(fn)});
  }
};

struct CheckFailure {};

#define TEST(name)                                        \
  static void test_##name();                              \
  static ::testing::Registrar reg_##name{#name, test_##name}; \
  static void test_##name()

#define CHECK(cond)                                                     \
  do {                                                                  \
    if (!(cond)) {                                                      \
      std::cerr << "    CHECK failed at " << __FILE__ << ":" << __LINE__ \
                << ": " #cond "\n";                                     \
      ::testing::failureCount()++;                                      \
      throw ::testing::CheckFailure{};                                  \
    }                                                                   \
  } while (0)

#define CHECK_NEAR(a, b, eps)                                            \
  do {                                                                   \
    double va = (a), vb = (b);                                           \
    if (!(std::fabs(va - vb) <= (eps))) {                                \
      std::cerr << "    CHECK_NEAR failed at " << __FILE__ << ":"        \
                << __LINE__ << ": " #a " = " << va << ", " #b " = " << vb \
                << "\n";                                                 \
      ::testing::failureCount()++;                                       \
      throw ::testing::CheckFailure{};                                   \
    }                                                                    \
  } while (0)

}  // namespace testing
