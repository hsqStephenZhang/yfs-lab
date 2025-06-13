// the caching lock server implementation

#include "lock_server_cache.h"
#include "lock_protocol.h"
#include "slock.h"
#include <algorithm>
#include <arpa/inet.h>
#include <pthread.h>
#include <sstream>
#include <stdio.h>
#include <unistd.h>

static void *revokethread(void *x) {
  lock_server_cache *sc = (lock_server_cache *)x;
  sc->revoker();
  return 0;
}

static void *retrythread(void *x) {
  lock_server_cache *sc = (lock_server_cache *)x;
  sc->retryer();
  return 0;
}

// get client while holding the server lock
rpcc *lock_server_cache::get_client_locked(std::string client) {
  if (clients.find(client) == clients.end()) {
    sockaddr_in dstsock;
    make_sockaddr(client.c_str(), &dstsock);
    rpcc *cl = new rpcc(dstsock);
    if (cl->bind() < 0) {
      printf("lock_server_cache: call bind\n");
    }
    clients[client] = cl;
  }
  return clients[client];
}

lock_server_cache::lock_server_cache() {
  pthread_t th;
  int r = pthread_mutex_init(&server_lock, NULL);
  assert(r == 0);
  r = pthread_create(&th, NULL, &revokethread, (void *)this);
  assert(r == 0);
  r = pthread_create(&th, NULL, &retrythread, (void *)this);
  assert(r == 0);
  r = pthread_cond_init(&revoke_queue_ready, NULL);
  assert(r == 0);
  r = pthread_cond_init(&retry_queue_ready, NULL);
  assert(r == 0);

  nacquire = 0;
  retry_queue = (std::queue<std::pair<lock_protocol::lockid_t, std::string>>());
  revoke_queue =
      (std::queue<std::pair<lock_protocol::lockid_t, std::string>>());
}

void lock_server_cache::revoker() {
  // This method should be a continuous loop, that sends revoke
  // messages to lock holders whenever another client wants the
  // same lock
  while (true) {
    // wait for a lock to be revoked
    pthread_mutex_lock(&server_lock);
    while (revoke_queue.empty()) {
      pthread_cond_wait(&revoke_queue_ready, &server_lock);
    }
    // hold server_lock now
    while (!revoke_queue.empty()) {
      auto pair = revoke_queue.front();
      revoke_queue.pop();
      // send revoke message to the client holding the lock
      auto client = this->get_client_locked(pair.second);
      auto lock_id = pair.first;
      auto tmp = 0;
      pthread_mutex_unlock(&server_lock);
      printf("[SERVER] send revoke of lock %lld to CLIENT-%s\n", lock_id,
             pair.second.c_str());
      auto res = client->call(rlock_protocol::revoke, lock_id, tmp);
      pthread_mutex_lock(&server_lock);
      DEBUG_ASSERT_EQ(res, lock_protocol::OK);
    }
    pthread_mutex_unlock(&server_lock);
  }
}

void lock_server_cache::retryer() {
  // This method should be a continuous loop, waiting for locks
  // to be released and then sending retry messages to those who
  // are waiting for it.
  while (true) {
    // wait for a lock to be retried
    pthread_mutex_lock(&server_lock);
    while (retry_queue.empty()) {
      pthread_cond_wait(&retry_queue_ready, &server_lock);
    }
    while (!retry_queue.empty()) {
      auto pair = retry_queue.front();
      retry_queue.pop();
      cached_lock &lock = this->locks[pair.first];
      lock.state = RETRYING;
      lock.clt = "";
      // send retry message to the client waiting for the lock
      auto lock_id = pair.first;
      auto tmp = 0;
      auto client = this->get_client_locked(pair.second);
      pthread_mutex_unlock(&server_lock);
      printf("[SERVER] send retry of lock %lld to CLIENT-%s\n", lock_id,
             pair.second.c_str());
      auto res = client->call(rlock_protocol::retry, lock_id, tmp);
      pthread_mutex_lock(&server_lock);
      DEBUG_ASSERT_EQ(res, lock_protocol::OK);
    }
    pthread_mutex_unlock(&server_lock);
  }
}

lock_protocol::status
lock_server_cache::stat(std::string clt, lock_protocol::lockid_t lid, int &r) {
  lock_protocol::status ret = lock_protocol::OK;
  printf("[SERVER] stat request from clt %s\n", clt.c_str());
  ScopedLock sl(&server_lock);
  r = nacquire;
  return ret;
}

lock_protocol::status lock_server_cache::release(std::string clt, int seq_num,
                                                 lock_protocol::lockid_t lid,
                                                 int &r) {
  ScopedLock sl(&server_lock);
  if (locks.find(lid) == locks.end()) {
    // lock not found, return error
    return lock_protocol::RPCERR;
  }
  cached_lock &cl = locks[lid];
  if (cl.clt != clt) {
    return lock_protocol::RPCERR;
  }

  printf("[SERVER] [RPC] release called by: %s, lid: %lld\n", clt.c_str(), lid);
  std::string retry_clt = "";

  switch (cl.state) {
  case EXCLUSIVE:
    DEBUG_ASSERT_EQ(cl.waiters.empty(), true);
    DEBUG_ASSERT_EQ(cl.clt == clt, true);
    cl.state = FREED;
    printf("[SERVER] release lock %lld released by client-%s\n", lid,
           clt.c_str());
    return lock_protocol::OK;
  case REVOKING:
    DEBUG_ASSERT_EQ(cl.waiters.empty(), false);
    DEBUG_ASSERT_EQ(cl.clt == clt, true);
    retry_clt = cl.waiters.front();
    retry_queue.push(std::pair(lid, retry_clt));
    pthread_cond_signal(&retry_queue_ready);
    printf("[SERVER] retrying lock %lld for client %s\n", lid,
           retry_clt.c_str());
    return lock_protocol::OK;
  default:
    // lock is not in a state to be released
    return lock_protocol::RPCERR;
  }
}

// TODO: use seq_num to prevent reordering
lock_protocol::status lock_server_cache::acquire(std::string clt, int seq_num,
                                                 lock_protocol::lockid_t lid,
                                                 int &r) {
  ScopedLock sl(&this->server_lock);

  if (locks.find(lid) == locks.end()) {
    locks.emplace(lid, cached_lock());
  }
  cached_lock &cl = locks[lid];
  printf("[SERVER] [RPC] acquire called by: %s, lid: %lld\n", clt.c_str(), lid);
  cl.debug();

  switch (cl.state) {
  case FREED:
    DEBUG_ASSERT_EQ(cl.clt, "");
    DEBUG_ASSERT_EQ(cl.waiters.empty(), true);
    cl.clt = clt;
    cl.state = EXCLUSIVE;
    printf("[SERVER] FREED->EXCLUSIVE %lld for client %s\n", lid, clt.c_str());
    return lock_protocol::OK;
  case EXCLUSIVE:
    // no re-entering
    DEBUG_ASSERT_EQ(cl.clt == clt, false);
    // state invariant
    DEBUG_ASSERT_EQ(cl.waiters.empty(), true);
    cl.waiters.push_back(clt);
    cl.state = REVOKING;
    printf("[SERVER] LOCKED %lld for client %s\n", lid,
           clt.c_str());
    cl.debug();
    this->revoke_queue.push(std::pair(lid, cl.clt));
    pthread_cond_signal(&revoke_queue_ready);
    break;
  case REVOKING:
    // there are onging revoke
    DEBUG_ASSERT_EQ(cl.waiters.empty(), false);
    assert(std::find(cl.waiters.begin(), cl.waiters.end(), clt) ==
           cl.waiters.end());
    cl.waiters.push_back(clt);
    printf("[SERVER] REVOKING->REVOKING %lld for client %s\n", lid,
           clt.c_str());
    break;
  case RETRYING:
    DEBUG_ASSERT_EQ(cl.clt, "");
    DEBUG_ASSERT_EQ(cl.waiters.empty(), false);
    if (cl.waiters.front() == clt) {
      cl.waiters.pop_front();
      cl.clt = clt;
      printf("[SERVER] LOCKED %lld for client %s\n", lid,
               clt.c_str());
      if (cl.waiters.empty()) {
        cl.state = EXCLUSIVE;
        printf("[SERVER] RETRYING->EXCLUSIVE %lld for client %s\n", lid,
               clt.c_str());
      } else {
        cl.state = REVOKING;
        // revoke imediately after setting the owner of the lock
        // for disable caching
        printf("[SERVER] RETRYING->REVOKING %lld for client %s\n", lid,
               clt.c_str());
        cl.debug();
        this->revoke_queue.push(std::pair(lid, clt));
        pthread_cond_signal(&revoke_queue_ready);
      }
      return lock_protocol::OK;
    } else {
      printf("[SERVER] RETRYING->REVOKING %lld for client %s\n", lid,
             clt.c_str());
      cl.waiters.push_back(clt);
    }
    break;
  }

  return lock_protocol::RETRY;
}