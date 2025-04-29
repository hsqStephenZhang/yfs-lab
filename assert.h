#include <iostream>
#include <cstdlib>
#include <execinfo.h>  // For backtrace
#include <unistd.h>

#define STACK_TRACE_SIZE 64

inline void print_stacktrace() {
    void* buffer[STACK_TRACE_SIZE];
    int nptrs = backtrace(buffer, STACK_TRACE_SIZE);
    char** symbols = backtrace_symbols(buffer, nptrs);
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

#define DEBUG_ASSERT(expr)                                        \
    do {                                                               \
        if (!(expr)) {                                                 \
            std::cerr << "Assertion failed: " << #expr << '\n';        \
            std::cerr << "In file: " << __FILE__                       \
                      << ", line: " << __LINE__ << '\n';               \
            print_stacktrace();                                        \
            std::abort();                                              \
        }                                                              \
    } while (0)
