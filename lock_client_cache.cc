// RPC stubs for clients to talk to lock_server, and cache the locks
// see lock_client.cache.h for protocol details.

#include "lock_client_cache.h"
#include "lock_protocol.h"
#include "rpc.h"
#include "rpc/slock.h"
#include "slock.h"
#include <atomic>
#include <iostream>
#include <pthread.h>
#include <sstream>
#include <stdio.h>

static void *releasethread(void *x) {
  lock_client_cache *cc = (lock_client_cache *)x;
  cc->releaser();
  return 0;
}

int lock_client_cache::last_port = 0;

lock_client_cache::lock_client_cache(std::string xdst,
                                     class lock_release_user *_lu)
    : lock_client(xdst), lu(_lu) {
  srand(time(NULL) ^ last_port);
  rlock_port = ((rand() % 32000) | (0x1 << 10));
  const char *hname;
  // assert(gethostname(hname, 100) == 0);
  hname = "127.0.0.1";
  std::ostringstream host;
  host << hname << ":" << rlock_port;
  id = host.str();
  last_port = rlock_port;
  rpcs *rlsrpc = new rpcs(rlock_port);
  /* register RPC handlers with rlsrpc */
  rlsrpc->reg(rlock_protocol::revoke, this, &lock_client_cache::revoke);
  rlsrpc->reg(rlock_protocol::retry, this, &lock_client_cache::retry);
  pthread_t th;
  int r = pthread_create(&th, NULL, &releasethread, (void *)this);
  pthread_mutex_init(&locks_mutex, NULL);
  pthread_mutex_init(&release_mutex, NULL);
  pthread_cond_init(&release_cond, NULL);
  assert(r == 0);
}

void lock_client_cache::releaser() {

  // This method should be a continuous loop, waiting to be notified of
  // freed locks that have been revoked by the server, so that it can
  // send a release RPC.
  while (true) {
    pthread_mutex_lock(&release_mutex);
    while (!release_queue.empty()) {
      auto lid = release_queue.front();
      release_queue.pop();

      lock_client_cache::lock_state &lock = this->get_lock(lid);
      pthread_mutex_unlock(&lock.mutex);
      /**
        lock_protocol::status acquire(std::string clt, int seq_num,
                                lock_protocol::lockid_t lid, int &r);
        lock_protocol::status release(std::string clt, int seq_num,
                                        lock_protocol::lockid_t lid, int &r);
      */
      auto tmp = 0;
      auto clt = std::string(this->id);
      this->cl->call(lock_protocol::release, clt, lock.seq_num, lid, tmp);
      pthread_mutex_lock(&lock.mutex);
      lock.status = lock_state::None; // reset the state
      lock.reset(std::atomic_fetch_add(&this->seq_num, 1));
      pthread_cond_broadcast(&lock.release_done);
    }
    pthread_cond_wait(&release_cond, &release_mutex);
  }
}

lock_client_cache::lock_state &
lock_client_cache::get_lock(lock_protocol::lockid_t lid) {
  ScopedLock sl(&locks_mutex);
  auto it = locks.find(lid);
  if (it == locks.end()) {
    unsigned int seq_num = std::atomic_fetch_add(&this->seq_num, 1);
    auto [inserted_it, success] = locks.emplace(lid, lock_state(seq_num));
    return inserted_it->second;
  }
  return it->second;
}

lock_protocol::status lock_client_cache::acquire(lock_protocol::lockid_t lid) {
  lock_protocol::status ret;
  lock_client_cache::lock_state &lock = this->get_lock(lid);
  pthread_mutex_lock(&lock.mutex);
  lock.debug();

  while (true) {
  loop:
    switch (lock.status) {
    case lock_state::None:
      lock.status = lock_state::Acquiring;
    retry_rpc: {
      pthread_mutex_unlock(&lock.mutex);
      auto tmp = 0;
      auto clt = std::string(this->id);
      ret = this->cl->call(lock_protocol::acquire, clt, lock.seq_num, lid, tmp);
      std::cout << "[DEBUG]: acquire rpc finished, res: " << ret << "\n"
                << std::endl;
      pthread_mutex_lock(&lock.mutex);
    }
      // 1. lock is issued by lock server
      if (ret == lock_protocol::OK) {
        lock.status = lock_state::Locked;
        auto tid = pthread_self();
        std::cout << "[DEBUG]: acquire lock " << lid
                  << " successfully, owner thread id: " << tid << std::endl;
        lock.owner_thread_id = tid;
        goto release;
      } else if (ret == lock_protocol::RETRY) {
        // the lock is not allocated to us, wait for retry and continue the loop
        // handle retry arrives before acquire returns
        if (!lock.no_wait) {
          pthread_cond_wait(&lock.retry_waiter, &lock.mutex);
        } else {
          // clear flag
          lock.no_wait = false;
        }
        goto retry_rpc;
      } else {
        // error
        goto release;
      }
      // loop retry
      goto loop;
      // 2. lock is issued by local cache
    case lock_state::Free:
      lock.status = lock_state::Locked;
      ret = lock_protocol::OK;
      lock.owner_thread_id = pthread_self();
      goto release;
    case lock_state::Acquiring:
    case lock_state::Locked:
      if (lock.no_cache) {
        pthread_cond_wait(&lock.release_done, &lock.mutex);
      } else {
        lock.local_waiter_cnt++;
        pthread_cond_wait(&lock.local_waiter, &lock.mutex);
        lock.local_waiter_cnt--;
      }

      // loop retry
      goto loop;
    case lock_state::Releasing:
      pthread_cond_wait(&lock.release_done, &lock.mutex);
      // loop retry
      goto loop;
    }
  }

release:
  pthread_mutex_unlock(&lock.mutex);

  return lock_protocol::RPCERR;
}

lock_protocol::status lock_client_cache::release(lock_protocol::lockid_t lid) {
  lock_client_cache::lock_state &lock = this->get_lock(lid);
  pthread_mutex_lock(&lock.mutex);

  if (lock.owner_thread_id != pthread_self()) {
    // this thread does not own the lock, return error
    std::cout
        << "[ERROR]: lock_client_cache::release: thread does not own the lock"
        << std::endl;
    pthread_mutex_unlock(&lock.mutex);
    return lock_protocol::RPCERR;
  }

  if (lock.status == lock_state::Locked) {
    lock.status = lock_state::Free;

    if (lock.no_cache && lock.local_waiter_cnt == 0) {
      lock.status = lock_state::Releasing;
      pthread_mutex_lock(&release_mutex);
      release_queue.push(lid);
      pthread_cond_signal(&release_cond);
      pthread_mutex_unlock(&release_mutex);
    } else {
      // TODO: local pending waiters first?
      // we could also return the lock immediately and then let local waiters
      // acquire the lock from server for better fairness
      if (lock.no_cache) {
        // signal the waiter
        pthread_cond_signal(&lock.local_waiter);
      } else if (lock.local_waiter_cnt > 0) {
        pthread_mutex_lock(&release_mutex);
        release_queue.push(lid);
        pthread_cond_signal(&release_cond);
        pthread_mutex_unlock(&release_mutex);
      }
    }
  } else {
    std::cout << "[ERROR]: lock_client_cache::release: lock is not locked"
              << std::endl;
  }

  pthread_mutex_unlock(&lock.mutex);
  return lock_protocol::OK;
}

// always return OK
rlock_protocol::status lock_client_cache::retry(lock_protocol::lockid_t lid,
                                                int &) {
  lock_client_cache::lock_state &lock = this->get_lock(lid);
  pthread_mutex_lock(&lock.mutex);
  lock.no_wait = true;

  pthread_cond_signal(&lock.retry_waiter);
  pthread_mutex_unlock(&lock.mutex);
  return rlock_protocol::OK;
}

// always return OK
rlock_protocol::status lock_client_cache::revoke(lock_protocol::lockid_t lid,
                                                 int &) {
  lock_client_cache::lock_state &lock = this->get_lock(lid);
  pthread_mutex_lock(&lock.mutex);
  // handle out of order of revoke
  lock.no_cache = true;
  if (lock.status == lock_state::Free) {
    lock.status = lock_state::Releasing;
    pthread_mutex_lock(&release_mutex);
    release_queue.push(lid);
    pthread_cond_signal(&release_cond);
    pthread_mutex_unlock(&release_mutex);
  }
  pthread_mutex_unlock(&lock.mutex);

  return rlock_protocol::OK;
}