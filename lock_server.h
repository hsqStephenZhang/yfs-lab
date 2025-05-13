// this is the lock server
// the lock client has a similar interface

#ifndef lock_server_h
#define lock_server_h

#include "lock_protocol.h"
#include <pthread.h>
#include <unordered_map>

enum lock_state { FREE = 1, LOCKED = 2 };

class lock_handle {
public:
  lock_state state;
  int clt;
  pthread_cond_t cond;

  lock_handle() : state(FREE), clt(-1) { pthread_cond_init(&cond, NULL); }
  lock_handle(int clt) : state(FREE), clt(clt) {
    pthread_cond_init(&cond, NULL);
  }
  ~lock_handle() { pthread_cond_destroy(&cond); }
};

class lock_server {
  pthread_mutex_t m;
  std::unordered_map<lock_protocol::lockid_t, lock_handle> locks;

protected:
  int nacquire;

public:
  lock_server();
  ~lock_server() {};
  lock_protocol::status stat(int clt, lock_protocol::lockid_t lid, int &);
  lock_protocol::status acquire(int clt, lock_protocol::lockid_t lid, int &r);
  lock_protocol::status release(int clt, lock_protocol::lockid_t lid, int &r);
};

#endif
