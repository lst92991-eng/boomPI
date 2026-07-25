#include "boompi/test/test_context.h"

#include <iostream>
#include <utility>

namespace boompi::test {

void TestContext::Expect(const bool condition, std::string expression,
                         const char* const file, const int line) {
  if (condition) {
    return;
  }
  ++failures_;
  std::cerr << file << ':' << line << ": expectation failed: " << expression
            << '\n';
}

std::size_t TestContext::failures() const noexcept { return failures_; }

}  // namespace boompi::test
