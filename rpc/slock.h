#ifndef __SCOPED_LOCK__
#define __SCOPED_LOCK__

#include <assert.h>
#include <cstdio>
#include <pthread.h>

struct ScopedLock {
private:
  pthread_mutex_t *m_;

public:
  ScopedLock(pthread_mutex_t *m) : m_(m) {
    int ret = pthread_mutex_lock(m_);
    if (ret != 0) {
      printf("pthread_mutex_lock failed with return value: %d\n", ret);
    }
    assert(ret == 0);
    // assert(pthread_mutex_lock(m_) == 0);
  }
  ~ScopedLock() { assert(pthread_mutex_unlock(m_) == 0); }
};
#endif /*__SCOPED_LOCK__*/
