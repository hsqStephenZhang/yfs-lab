#include <algorithm>
#include <chrono>
#include <ctime>

#ifndef NOW_H
#define NOW_H

inline uint64_t next_time(unsigned int prev) {
  unsigned int now = time(0);
  return std::max(now, prev + 1);
}

#endif // NOW_H