#ifndef lock_server_cache_h
#define lock_server_cache_h

#include "lock_protocol.h"
#include "lock_server.h"
#include "rpc.h"
#include "utils.h"
#include <cassert>
#include <iostream>
#include <map>
#include <pthread.h>
#include <queue>
#include <string>
#include <utility>

// use when either holder or waiters is not empty
// exclusive: one client holds the lock, no waiter, so the lock is also cached
// revoking: one client holds the cached lock, there are many waiters, but we
// have sent the revoke cmd revoked: no client holds the cached lock,
// preparation for retry retrying: no client holds the cached lock, we have sent
// the retry cmd to the first waiter
enum cached_state { FREED, EXCLUSIVE = 1, REVOKING = 2, RETRYING = 3 };

// if it's server, then
// free -> exclusive
// exclusive -> revoking
// revoking -> retrying
// retrying -> exclusive
// exclusive -> free (rare) considered as warning

class cached_lock {
public:
  cached_state state;
  // holder of the lock, if 0, then no one has it
  std::string clt;
  // waiters
  std::list<std::string> waiters;
  cached_lock() : state(FREED), clt(""), waiters({}) {}

  void debug() {
    std::cout << "[SERVER] cached_lock debug info:" << std::endl;
    std::cout << "[SERVER] cached_lock state: " << state << ", clt: " << clt
              << ", waiters: ";
    for (const auto &w : waiters) {
      std::cout << w << " ";
    }
    std::cout << std::endl << std::endl;
  }
};

class lock_server_cache : lock_server {
private:
  int nacquire;
  pthread_mutex_t server_lock;

  std::map<lock_protocol::lockid_t, cached_lock> locks;
  std::map<std::string, rpcc *> clients;

  // add other member variables as needed
  // for example, a list of locks that are being revoked
  // and a list of locks that are being retried
public:
  lock_server_cache();
  ~lock_server_cache() {};
  rpcc *get_client_locked(std::string);
  lock_protocol::status stat(std::string clt, lock_protocol::lockid_t lid,
                             int &);
  lock_protocol::status acquire(std::string clt, int seq_num,
                                lock_protocol::lockid_t lid, int &r);
  lock_protocol::status release(std::string clt, int seq_num,
                                lock_protocol::lockid_t lid, int &r);

  pthread_cond_t revoke_queue_ready;
  pthread_cond_t retry_queue_ready;
  // first in first out
  std::queue<std::pair<lock_protocol::lockid_t, std::string>> revoke_queue;
  // first in first out
  std::queue<std::pair<lock_protocol::lockid_t, std::string>> retry_queue;

  void revoker();
  void retryer();
};

#endif
