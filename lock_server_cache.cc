// the caching lock server implementation

#include "lock_server_cache.h"
#include "tprintf.h"
#include "utils.h"
#include <arpa/inet.h>
#include <sstream>
#include <stdio.h>
#include <unistd.h>

lock_server_cache::lock_server_cache() {
  nacquire = 0;
  pthread_mutex_init(&m, NULL);
}

lock_server_cache::~lock_server_cache() {
  pthread_mutex_destroy(&m);

  // 清理锁条目
  for (auto &pair : locks) {
    delete pair.second;
  }

  // 清理客户端连接
  for (auto &pair : clients) {
    delete pair.second;
  }
}

lock_server_cache::server_lock_entry *
lock_server_cache::get_server_lock_entry(lock_protocol::lockid_t lid) {
  if (locks.find(lid) == locks.end()) {
    locks[lid] = new server_lock_entry();
  }
  return locks[lid];
}

rpcc *lock_server_cache::get_client_connection(const std::string &client_id) {
  if (clients.find(client_id) == clients.end()) {
    // 解析客户端地址
    size_t pos = client_id.find(':');
    if (pos == std::string::npos) {
      return NULL;
    }

    sockaddr_in dstsock;
    make_sockaddr(client_id.c_str(), &dstsock);

    clients[client_id] = new rpcc(dstsock);
  }

  return clients[client_id];
}

void lock_server_cache::send_revoke(lock_protocol::lockid_t lid,
                                    const std::string &client_id) {
  rpcc *client = get_client_connection(client_id);
  if (client) {
    int r;
    // 异步发送revoke消息
    client->call(rlock_protocol::revoke, lid, r);
  }
}

void lock_server_cache::send_retry(lock_protocol::lockid_t lid,
                                   const std::string &client_id) {
  rpcc *client = get_client_connection(client_id);
  if (client) {
    int r;
    // 异步发送retry消息
    client->call(rlock_protocol::retry, lid, r);
  }
}

lock_protocol::status lock_server_cache::stat(lock_protocol::lockid_t lid,
                                              int &r) {
  lock_protocol::status ret = lock_protocol::OK;

  pthread_mutex_lock(&m);
  r = nacquire;
  pthread_mutex_unlock(&m);

  return ret;
}

lock_protocol::status lock_server_cache::acquire(lock_protocol::lockid_t lid,
                                                 std::string id, int &r) {
  pthread_mutex_lock(&m);

  server_lock_entry *entry = get_server_lock_entry(lid);

  switch (entry->state) {
  case FREE_SERVER:
    // 锁空闲，直接分配给请求者
    entry->state = LOCKED_SERVER;
    entry->owner = id;
    entry->revoke_sent = false;
    nacquire++;
    pthread_mutex_unlock(&m);
    return lock_protocol::OK;

  case LOCKED_SERVER:
    if (entry->owner == id) {
      // 同一客户端重复请求
      pthread_mutex_unlock(&m);
      return lock_protocol::OK;
    }

    // 锁被其他客户端占用
    entry->waiters.push(id);

    if (!entry->revoke_sent) {
      // 第一次有人等待，发送revoke给当前拥有者
      entry->revoke_sent = true;
      std::string owner = entry->owner;
      pthread_mutex_unlock(&m);

      // 发送revoke消息
      send_revoke(lid, owner);

      pthread_mutex_lock(&m);
    }

    pthread_mutex_unlock(&m);
    return lock_protocol::RETRY;
  }

  pthread_mutex_unlock(&m);
  return lock_protocol::IOERR;
}

lock_protocol::status lock_server_cache::release(lock_protocol::lockid_t lid,
                                                 std::string id, int &r) {
  pthread_mutex_lock(&m);

  auto it = locks.find(lid);
  if (it == locks.end()) {
    pthread_mutex_unlock(&m);
    return lock_protocol::NOENT;
  }

  server_lock_entry *entry = it->second;

  if (entry->state != LOCKED_SERVER || entry->owner != id) {
    pthread_mutex_unlock(&m);
    return lock_protocol::NOENT;
  }

  if (entry->waiters.empty()) {
    // 没有等待者，锁变为空闲状态
    entry->state = FREE_SERVER;
    entry->owner = "";
    entry->revoke_sent = false;
  } else {
    // 有等待者，将锁分配给下一个等待者
    std::string next_client = entry->waiters.front();
    entry->waiters.pop();

    entry->owner = next_client;
    entry->revoke_sent = false;

    // 如果还有其他等待者，为新拥有者设置revoke标志
    if (!entry->waiters.empty()) {
      entry->revoke_sent = true;
    }

    // 通知新的拥有者可以重试
    pthread_mutex_unlock(&m);
    send_retry(lid, next_client);
    pthread_mutex_lock(&m);

    // 如果有更多等待者，立即发送revoke给新拥有者
    if (entry->revoke_sent) {
      std::string new_owner = entry->owner;
      pthread_mutex_unlock(&m);
      send_revoke(lid, new_owner);
      pthread_mutex_lock(&m);
    }
  }

  pthread_mutex_unlock(&m);
  return lock_protocol::OK;
}