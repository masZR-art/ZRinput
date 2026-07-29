#pragma once

#include <exception>
#include <functional>
#include <iostream>
#include <sstream>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

namespace zrinput::test {

class Failure : public std::runtime_error {
 public:
  using std::runtime_error::runtime_error;
};

using Test = std::pair<std::string, std::function<void()>>;

inline std::vector<Test>& Registry() {
  static std::vector<Test> tests;
  return tests;
}

class Registration {
 public:
  Registration(std::string name, std::function<void()> test) {
    Registry().emplace_back(std::move(name), std::move(test));
  }
};

template <typename Actual, typename Expected>
void ExpectEqual(const Actual& actual,
                 const Expected& expected,
                 const char* actual_expression,
                 const char* expected_expression,
                 const char* file,
                 int line) {
  if (!(actual == expected)) {
    std::ostringstream message;
    message << file << ':' << line << ": expected " << actual_expression
            << " == " << expected_expression;
    throw Failure(message.str());
  }
}

inline void ExpectTrue(bool value,
                       const char* expression,
                       const char* file,
                       int line) {
  if (!value) {
    std::ostringstream message;
    message << file << ':' << line << ": expected true: " << expression;
    throw Failure(message.str());
  }
}

inline int RunAll() {
  std::size_t failed = 0;
  for (const auto& [name, test] : Registry()) {
    try {
      test();
      std::cout << "[PASS] " << name << '\n';
    } catch (const std::exception& error) {
      ++failed;
      std::cerr << "[FAIL] " << name << ": " << error.what() << '\n';
    } catch (...) {
      ++failed;
      std::cerr << "[FAIL] " << name << ": unknown exception\n";
    }
  }
  std::cout << (Registry().size() - failed) << '/' << Registry().size()
            << " tests passed\n";
  return failed == 0 ? 0 : 1;
}

}  // namespace zrinput::test

#define ZR_TEST(name)                                                        \
  static void name();                                                        \
  static ::zrinput::test::Registration registration_##name(#name, name);     \
  static void name()

#define ZR_EXPECT_TRUE(expression)                                           \
  ::zrinput::test::ExpectTrue(static_cast<bool>(expression), #expression,    \
                              __FILE__, __LINE__)

#define ZR_EXPECT_EQ(actual, expected)                                       \
  ::zrinput::test::ExpectEqual((actual), (expected), #actual, #expected,     \
                               __FILE__, __LINE__)

