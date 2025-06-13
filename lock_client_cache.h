// lock client interface.

#ifndef lock_client_cache_h

#define lock_client_cache_h

#include "lock_client.h"
#include "lock_protocol.h"
#include "lock_server.h"
#include "rpc.h"
#include <atomic>
#include <pthread.h>
#include <queue>
#include <string>

// Classes that inherit lock_release_user can override dorelease so that
// that they will be called when lock_client releases a lock.
// You will not need to do anything with this class until Lab 6.
class lock_release_user {
public:
  virtual void dorelease(lock_protocol::lockid_t) = 0;
  virtual ~lock_release_user() {};
};

class dummy_lock_release_user : public lock_release_user {
public:
  void dorelease(lock_protocol::lockid_t) override {
    printf("DUMMY dorelease called\n");
    // do nothing
  }
  ~dummy_lock_release_user() {};
};

// SUGGESTED LOCK CACHING IMPLEMENTATION PLAN:
//
// to work correctly for lab 7,  all the requests on the server run till
// completion and threads wait on condition variables on the client to
// wait for a lock.  this allows the server to be replicated using the
// replicated state machine approach.
//
// On the client a lock can be in several states:
//  - free: client owns the lock and no thread has it
//  - locked: client owns the lock and a thread has it
//  - acquiring: the client is acquiring ownership
//  - releasing: the client is releasing ownership
//
// in the state acquiring and locked there may be several threads
// waiting for the lock, but the first thread in the list interacts
// with the server and wakes up the threads when its done (released
// the lock).  a thread in the list is identified by its thread id
// (tid).
//
// a thread is in charge of getting a lock: if the server cannot grant
// it the lock, the thread will receive a retry reply.  at some point
// later, the server sends the thread a retry RPC, encouraging the client
// thread to ask for the lock again.
//
// once a thread has acquired a lock, its client obtains ownership of
// the lock. the client can grant the lock to other threads on the client
// without interacting with the server.
//
// the server must send the client a revoke request to get the lock back. this
// request tells the client to send the lock back to the
// server when the lock is released or right now if no thread on the
// client is holding the lock.  when receiving a revoke request, the
// client adds it to a list and wakes up a releaser thread, which returns
// the lock the server as soon it is free.
//
// the releasing is done in a separate a thread to avoid
// deadlocks and to ensure that revoke and retry RPCs from the server
// run to completion (i.e., the revoke RPC cannot do the release when
// the lock is free.
//
// a challenge in the implementation is that retry and revoke requests
// can be out of order with the acquire and release requests.  that
// is, a client may receive a revoke request before it has received
// the positive acknowledgement on its acquire request.  similarly, a
// client may receive a retry before it has received a response on its
// initial acquire request.  a flag field is used to record if a retry
// has been received.
//

class lock_client_cache : public lock_client {
  struct lock_state {

    enum Status {
      None = 0,
      Free = 1,
      Acquiring = 2,
      // use other fields to indicate this state
      // AcquiringWaiting, // == Acquiring + RETRY resp, waiting for server's
      // retry req
      Locked = 3,
      // use other fields to indicate this state
      // LockedNoCache, // == Locked + REVOKE flag, should not cache the lock,
      // return it immediately
      Releasing = 4,
    };

    pthread_mutex_t mutex;

    int local_waiter_cnt;
    // multiple local waiter on acquiring state
    pthread_cond_t local_waiter;
    // only one thread owns the lock, which could be invoked by server's retry
    pthread_cond_t retry_waiter;
    pthread_cond_t release_done;
    Status status = None;
    pthread_t owner_thread_id;
    // handle server revoke cmd out of order
    bool no_cache;
    // handle server retry cmd out of order
    bool retry_token;
    unsigned int seq_num;

    lock_state(int seq_num)
        : local_waiter_cnt(0), status(None), owner_thread_id(0),
          no_cache(false), retry_token(false), seq_num(seq_num) {
      pthread_mutex_init(&mutex, NULL);
      pthread_cond_init(&local_waiter, NULL);
      pthread_cond_init(&retry_waiter, NULL);
      pthread_cond_init(&release_done, NULL);
    }

    lock_state() : lock_state(0) {}

    void reset(int seq_num) {
      status = None;
      this->seq_num = seq_num;
    }

    void debug(const char *id) {
      printf("[CLIENT-%s]lock_state debug info:\n", id);
      std::cout << "status: " << status
                << ", owner_thread_id: " << owner_thread_id
                << ", local_waiter_cnt: " << local_waiter_cnt
                << ", no_cache: " << no_cache
                << ", no_wait: " << retry_token
                << ", seq_num: " << seq_num << std::endl;
    }

    ~lock_state() { pthread_mutex_destroy(&mutex); }
  };

private:
  class lock_release_user *lu;
  int rlock_port;
  std::string hostname;
  std::string id;

  // sequence number per lock client(we could also make it per lock)
  std::atomic<unsigned int> seq_num;
  // mutex for insert new lock into the locks
  // every lock object has its own mutex (per map slot)
  pthread_mutex_t locks_mutex;
  std::map<lock_protocol::lockid_t, lock_state> locks;
  pthread_mutex_t release_mutex;
  pthread_cond_t release_cond;
  std::queue<lock_protocol::lockid_t> release_queue;

public:
  static int last_port;
  lock_client_cache(std::string xdst, class lock_release_user *l = 0);
  virtual ~lock_client_cache() {};
  lock_client_cache::lock_state &get_lock(lock_protocol::lockid_t);
  lock_protocol::status acquire(lock_protocol::lockid_t);
  lock_protocol::status release(lock_protocol::lockid_t);
  std::string get_id() {
    return this->id;
  }
  void print_id() {
    std::cout << "[CLIENT-" << this->id << "]";
  }

  rlock_protocol::status retry(lock_protocol::lockid_t, int &);
  rlock_protocol::status revoke(lock_protocol::lockid_t, int &);
};
#endif
