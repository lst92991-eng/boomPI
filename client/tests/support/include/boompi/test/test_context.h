#ifndef BOOMPI_TEST_TEST_CONTEXT_H_
#define BOOMPI_TEST_TEST_CONTEXT_H_

#include <cstddef>
#include <string>

namespace boompi::test {

class TestContext final {
 public:
  void Expect(bool condition, std::string expression, const char* file,
              int line);
  std::size_t failures() const noexcept;

 private:
  std::size_t failures_{0};
};

}  // namespace boompi::test

#endif  // BOOMPI_TEST_TEST_CONTEXT_H_
