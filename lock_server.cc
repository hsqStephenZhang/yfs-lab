// the lock server implementation

#include "lock_server.h"
#include "rpc/slock.h"
#include <arpa/inet.h>
#include <pthread.h>
#include <stdio.h>
#include <unistd.h>

lock_server::lock_server() : nacquire(0) { pthread_mutex_init(&m, NULL); }

lock_protocol::status lock_server::stat(int clt, lock_protocol::lockid_t lid,
                                        int &r) {
  lock_protocol::status ret = lock_protocol::OK;
  printf("stat request from clt %d\n", clt);
  ScopedLock sl(&m);
  r = nacquire;
  return ret;
}

lock_protocol::status lock_server::acquire(int clt, lock_protocol::lockid_t lid,
                                           int &r) {
  ScopedLock sl(&m);

  if (locks.find(lid) == locks.end()) {
    locks.emplace(lid, lock_handle());
  }
  lock_handle &lh = locks[lid];

  // wait till lh.state == FREE
  while (lh.state != lock_state::FREE) {
    pthread_cond_wait(&lh.cond, &m);
  }

  // we have the lock now
  nacquire++;
  lh.state = lock_state::LOCKED;
  lh.clt = clt;

  printf("acquire request for lock %016llx from clt %d\n", lid, clt);
  return lock_protocol::OK;
}

lock_protocol::status lock_server::release(int clt, lock_protocol::lockid_t lid,
                                           int &r) {
  ScopedLock sl(&m);

  if (locks.find(lid) != locks.end()) {
    lock_handle &lh = locks.at(lid);
    if (lh.clt != clt) {
      // lock held by another client or lock not held
      printf(
          "release request for lock %016llx from clt %d, but held by clt %d\n",
          lid, clt, lh.clt);
      return lock_protocol::RPCERR;
    }

    lh.state = lock_state::FREE;
    lh.clt = -1;

    // notify one waiting thread
    pthread_cond_signal(&lh.cond);
    nacquire--;

    printf("release request for lock %016llx from clt %d\n", lid, clt);
    return lock_protocol::OK;
  }

  // lock not found
  printf("release request for lock %016llx from clt %d, but lock not found\n",
         lid, clt);
  return lock_protocol::RPCERR;
}
