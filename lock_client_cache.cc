// RPC stubs for clients to talk to lock_server, and cache the locks
// see lock_client.cache.h for protocol details.

#include "lock_client_cache.h"
#include "lock_protocol.h"
#include "rpc.h"
#include "rpc/slock.h"
#include "slock.h"
#include "utils.h"
#include <atomic>
#include <iostream>
#include <pthread.h>
#include <sstream>
#include <stdio.h>

int lock_client_cache::last_port = 0;

lock_client_cache::lock_client_cache(std::string xdst,
                                     class lock_release_user *_lu)
    : lock_client(xdst), lu(_lu) {
  srand(time(NULL) ^ last_port);
  rlock_port = ((rand() % 32000) | (0x1 << 10));
  const char *hname;
  hname = "127.0.0.1";
  std::ostringstream host;
  host << hname << ":" << rlock_port;
  id = host.str();
  last_port = rlock_port;
  rpcs *rlsrpc = new rpcs(rlock_port);
  /* register RPC handlers with rlsrpc */
  rlsrpc->reg(rlock_protocol::revoke, this, &lock_client_cache::revoke);
  rlsrpc->reg(rlock_protocol::retry, this, &lock_client_cache::retry);
  pthread_mutex_init(&locks_mutex, NULL);
  pthread_mutex_init(&release_mutex, NULL);
  pthread_cond_init(&release_cond, NULL);
}

lock_client_cache::lock_state &
lock_client_cache::get_lock(lock_protocol::lockid_t lid) {
  ScopedLock sl(&locks_mutex);
  auto it = locks.find(lid);
  if (it == locks.end()) {
    unsigned int seq_num = std::atomic_fetch_add(&this->seq_num, 1);
    auto [inserted_it, success] = locks.emplace(lid, lock_state(seq_num));
    printf("[CLIENT-%s]get_lock LOCK-%lld not found, creating new lock, lock: "
           "%p\n",
           this->id.c_str(), lid, &inserted_it->second);
    return inserted_it->second;
  }
  return it->second;
}

lock_protocol::status lock_client_cache::acquire(lock_protocol::lockid_t lid) {
  lock_protocol::status ret;
  lock_client_cache::lock_state &lock = this->get_lock(lid);
  pthread_mutex_lock(&lock.mutex);
  printf("[CLIENT-%s]acquire LOCK-%lld\n", this->id.c_str(), lid);
  lock.debug(this->id.c_str());

  while (true) {
  loop:
    switch (lock.status) {
    case lock_state::None:
      lock.status = lock_state::Acquiring;
    retry_rpc: {
      pthread_mutex_unlock(&lock.mutex);
      auto tmp = 0;
      auto clt = std::string(this->id);
      printf("[CLIENT-%s]acquire LOCK-%lld to server\n", this->id.c_str(), lid);
      ret = this->cl->call(lock_protocol::acquire, clt, lock.seq_num, lid, tmp);
      printf("[CLIENT-%s]acquire LOCK-%lld rpc returned %d\n", this->id.c_str(),
             lid, ret);
      pthread_mutex_lock(&lock.mutex);
    }
      // 1. lock is issued by lock server
      if (ret == lock_protocol::OK) {
        lock.status = lock_state::Locked;
        auto tid = pthread_self();
        printf("[CLIENT-%s]acquire LOCK-%lld, owner thread id: %ld, lock: %p\n",
               this->id.c_str(), lid, tid, &lock);
        lock.owner_thread_id = tid;
        // we are good
        goto release;
      } else if (ret == lock_protocol::RETRY) {
        // the lock is not allocated to us, wait for retry and continue the loop
        // handle retry arrives before acquire returns
        if (!lock.retry_token) {
          printf("[CLIENT-%s]acquire LOCK-%lld, waiting for retry\n",
                 this->id.c_str(), lid);
          pthread_cond_wait(&lock.retry_waiter, &lock.mutex);
        }
        DEBUG_ASSERT_EQ(lock.retry_token, true);
        lock.retry_token = false;
        // now the retry token is consumed, we are still the only one
        // who could issue the rpc call
        goto retry_rpc;
      } else {
        // error
        goto release;
      }
      // 2. lock is issued by local cache
    case lock_state::Free:
      lock.status = lock_state::Locked;
      ret = lock_protocol::OK;
      lock.owner_thread_id = pthread_self();
      printf("[CLIENT-%s]acquire LOCK-%lld, owner thread id: %ld, lock: %p\n",
             this->id.c_str(), lid, lock.owner_thread_id, &lock);
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

  return lock_protocol::OK;
}

lock_protocol::status lock_client_cache::release(lock_protocol::lockid_t lid) {
  lock_client_cache::lock_state &lock = this->get_lock(lid);
  pthread_mutex_lock(&lock.mutex);

  if (lock.owner_thread_id != pthread_self()) {
    // this thread does not own the lock, return error
    printf("[CLIENT-%s]release LOCK-%lld, but not owner, owner: %ld, current "
           "%ld, lock: %p\n",
           this->id.c_str(), lid, lock.owner_thread_id, pthread_self(), &lock);
    pthread_mutex_unlock(&lock.mutex);
    return lock_protocol::RPCERR;
  }

  printf("[CLIENT-%s]release LOCK-%lld\n", this->id.c_str(), lid);
  lock.debug(this->id.c_str());

  if (lock.status == lock_state::Locked) {
    lock.status = lock_state::Free;
    lock.owner_thread_id = 0; // reset the owner thread id

    if (lock.local_waiter_cnt > 0) {
      // local waiters requested the lock before revoke cmd
      // so have higher priority
      printf("[CLIENT-%s]release LOCK-%lld, notify local waiters\n",
             this->id.c_str(), lid);
      pthread_cond_signal(&lock.local_waiter);
    } else if (lock.no_cache) {
      lock.status = lock_state::Releasing;
      printf("[CLIENT-%s]release LOCK-%lld, notify server to release\n",
             this->id.c_str(), lid);
      pthread_mutex_unlock(&lock.mutex);
      auto tmp = 0;
      auto clt = std::string(this->id);
      this->lu->dorelease(lid);
      this->cl->call(lock_protocol::release, clt, lock.seq_num, lid, tmp);
      pthread_mutex_lock(&lock.mutex);
      lock.status = lock_state::None; // reset the state
      lock.no_cache = false;
      pthread_cond_broadcast(&lock.release_done);
    } else {
      printf("[CLIENT-%s]release LOCK-%lld, still cached\n", this->id.c_str(),
             lid);
    }
  } else {
    printf("[CLIENT-%s]release LOCK-%lld, but not in Locked state, state: %d\n",
           this->id.c_str(), lid, lock.status);
  }

  pthread_mutex_unlock(&lock.mutex);
  printf("[CLIENT-%s]release done LOCK-%lld\n", this->id.c_str(), lid);
  return lock_protocol::OK;
}

// always return OK
rlock_protocol::status lock_client_cache::retry(lock_protocol::lockid_t lid,
                                                int &) {
  lock_client_cache::lock_state &lock = this->get_lock(lid);
  pthread_mutex_lock(&lock.mutex);
  lock.retry_token = true;
  printf("[CLIENT-%s] retry of lock: %lld, status: %d, local_waiter_cnt: %d\n",
         this->id.c_str(), lid, lock.status, lock.local_waiter_cnt);

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
  printf("[CLIENT-%s] revoke of lock: %lld, status: %d, local_waiter_cnt: %d\n",
         this->id.c_str(), lid, lock.status, lock.local_waiter_cnt);
  lock.no_cache = true;
  if (lock.status == lock_state::Free) {
    lock.status = lock_state::Releasing;
    pthread_mutex_unlock(&lock.mutex);
    auto tmp = 0;
    auto clt = std::string(this->id);
    this->lu->dorelease(lid);
    this->cl->call(lock_protocol::release, clt, lock.seq_num, lid, tmp);
    pthread_mutex_lock(&lock.mutex);
    lock.status = lock_state::None; // reset the state
    lock.no_cache = false;
    pthread_cond_broadcast(&lock.release_done);
  }
  pthread_mutex_unlock(&lock.mutex);

  return rlock_protocol::OK;
}