#ifndef DEBUG_UTILS_H
#define DEBUG_UTILS_H

#include <cstdlib>
#include <execinfo.h> // For backtrace
#include <iostream>
#include <unistd.h>

#ifdef BACKTRACE

#define STACK_TRACE_SIZE 64

template <typename T> void print_expr_value(const T &value) {
  if constexpr (std::is_same_v<decltype(std::cerr << value), std::ostream &>) {
    std::cerr << "Value: " << value << '\n';
  } else {
    std::cerr << "Value: [unprintable type]\n";
  }
}

template <typename T> void print_value(const char *name, const T &value) {
  std::cerr << name << " = ";
  if constexpr (std::is_same_v<decltype(std::cerr << value), std::ostream &>) {
    std::cerr << value;
  } else {
    std::cerr << "[unprintable type]";
  }
  std::cerr << '\n';
}

inline void print_stacktrace() {
  void *buffer[STACK_TRACE_SIZE];
  int nptrs = backtrace(buffer, STACK_TRACE_SIZE);
  char **symbols = backtrace_symbols(buffer, nptrs);
  if (symbols != nullptr) {
    std::cerr << "Stack trace:\n";
    for (int i = 0; i < nptrs; ++i) {
      std::cerr << symbols[i] << '\n';
    }
    free(symbols);
  } else {
    std::cerr << "Failed to generate stack trace.\n";
  }
}

#define DEBUG_ASSERT_EQ(left, right)                                           \
  do {                                                                         \
    auto &&_left_val = (left);                                                 \
    auto &&_right_val = (right);                                               \
    if (!(_left_val == _right_val)) {                                          \
      std::cerr << "Assertion failed: " << #left << " == " << #right << '\n';  \
      std::cerr << "In file: " << __FILE__ << ", line: " << __LINE__ << '\n';  \
      print_value(#left, _left_val);                                           \
      print_value(#right, _right_val);                                         \
      print_stacktrace();                                                      \
      std::abort();                                                            \
    }                                                                          \
  } while (0)

#else

#define DEBUG_ASSERT_EQ(left, right)                                           \
  do {                                                                         \
    assert((left) == (right));                                                 \
  } while (0)

#endif

#endif // DEBUG_UTILS_H