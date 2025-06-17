#include <chrono>

#ifndef NOW_H
#define NOW_H

inline uint64_t get_now() {
  using namespace std::chrono;
  return duration_cast<milliseconds>(system_clock::now().time_since_epoch())
      .count();
}

#endif // NOW_H