#include "lock_client_cache.h"
#include "lock_server_cache.h"
#include <gtest/gtest.h>
#include <pthread.h>
#include <string.h>
#include <thread>
#include <unistd.h>

const int nt = 5;
lock_client_cache **lc = new lock_client_cache *[nt];
lock_protocol::lockid_t a = 1;
lock_protocol::lockid_t b = 2;
lock_protocol::lockid_t c = 3;

void *test2(void *x) {
  int i = *(int *)x;

  printf("client %d acquire\n", i);
  lc[i]->acquire(a);
  printf("client %d acquire done, thread id: %ld\n", i, pthread_self());
  sleep(1);
  printf("client %d release\n", i);
  lc[i]->release(a);
  printf("client %d release done, thread id: %ld\n", i, pthread_self());
  return 0;
}

void *test3(void *x) {
  int i = *(int *)x;

  printf("test3: client %d acquire a release a concurrent\n", i);
  for (int j = 0; j < 10; j++) {
    lc[i]->acquire(a);
    printf("test3: client %d got lock\n", i);
    lc[i]->release(a);
  }
  return 0;
}

void *test4(void *x) {
  int i = *(int *)x;

  printf("test4: thread %d acquire a release a concurrent; same clnt\n", i);
  for (int j = 0; j < 10; j++) {
    lc[0]->acquire(a);
    printf("test4: thread %d on client 0 got lock\n", i);
    lc[0]->release(a);
  }
  return 0;
}

void *test5_inner(void *x) {
  int i = *(int *)x;
  auto tid = pthread_self();
  printf("test5 inner: thread %ld on client %d\n", tid, i);
  for (int j = 0; j < 10; j++) {
    lc[i]->acquire(a);
    std::this_thread::sleep_for(std::chrono::milliseconds(100));
    lc[i]->release(a);
  }
  return 0;
}

void *test5(void *x) {
  int i = *(int *)x;

  printf("test5: thread %d acquire a release a concurrent\n", i);

  int num_local = 10;

  pthread_t locals[num_local];
  for (int i = 0; i < num_local; i++) {
    auto r = pthread_create(&locals[i], NULL, test5_inner, x);
    assert(r == 0);
  }

  // Wait for all threads to complete
  for (int i = 0; i < num_local; i++) {
    auto r = pthread_join(locals[i], NULL);
    assert(r == 0);
  }

  return 0;
}

TEST(SimpleTest, t) {

  lock_server_cache ls;
  rpcs server(3000, 0);
  server.reg(lock_protocol::stat, &ls, &lock_server_cache::stat);
  server.reg(lock_protocol::acquire, &ls, &lock_server_cache::acquire);
  server.reg(lock_protocol::release, &ls, &lock_server_cache::release);

  for (int i = 0; i < nt; i++) {
    lc[i] = new lock_client_cache("3000", new dummy_lock_release_user());
    printf("lc[%d]: %s\n", i, lc[i]->get_id().c_str());
  }

  pthread_t th[nt];
  for (int i = 0; i < nt; i++) {
    int *a = new int(i);
    auto r = pthread_create(&th[i], NULL, test5, (void *)a);
    assert(r == 0);
  }

  // Wait for all threads to complete
  for (int i = 0; i < nt; i++) {
    auto r = pthread_join(th[i], NULL);
    assert(r == 0);
  }

  printf("All threads completed\n");
}