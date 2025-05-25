// RPC stubs for clients to talk to lock_server, and cache the locks
// see lock_client.cache.h for protocol details.

#include "lock_client_cache.h"
#include "rpc.h"
#include "tprintf.h"
#include <iostream>
#include <sstream>
#include <stdio.h>

int lock_client_cache::last_port = 0;

// 实现部分
lock_client_cache::lock_client_cache(std::string xdst, 
                                   class lock_release_user *_lu)
  : lock_client(xdst), lu(_lu)
{
  srand(time(NULL)^last_port);
  rlock_port = ((rand()%32000) | (0x1 << 10));
  const char *hname;
  
  // 获取主机名
  hname = "127.0.0.1";
  std::ostringstream host;
  host << hname << ":" << rlock_port;
  id = host.str();
  last_port = rlock_port;
  
  pthread_mutex_init(&m, NULL);
  
  // 启动RPC服务器用于接收revoke/retry回调
  rpcs *rlsrpc = new rpcs(rlock_port);
  rlsrpc->reg(rlock_protocol::revoke, this, &lock_client_cache::revoke_handler);
  rlsrpc->reg(rlock_protocol::retry, this, &lock_client_cache::retry_handler);
}

lock_client_cache::~lock_client_cache()
{
  pthread_mutex_destroy(&m);
  for (auto& pair : locks) {
    delete pair.second;
  }
}

lock_client_cache::lock_entry* 
lock_client_cache::get_lock_entry(lock_protocol::lockid_t lid)
{
  if (locks.find(lid) == locks.end()) {
    locks[lid] = new lock_entry();
  }
  return locks[lid];
}

void lock_client_cache::remove_lock_entry(lock_protocol::lockid_t lid)
{
  auto it = locks.find(lid);
  if (it != locks.end()) {
    delete it->second;
    locks.erase(it);
  }
}

lock_protocol::status
lock_client_cache::acquire(lock_protocol::lockid_t lid)
{
  pthread_mutex_lock(&m);
  
  lock_entry *entry = get_lock_entry(lid);
  
  while (true) {
    switch (entry->state) {
      case NONE:
        // 第一次获取此锁，需要从服务器获取
        entry->state = ACQUIRING;
        pthread_mutex_unlock(&m);
        
        {
          int r;
          lock_protocol::status ret = cl->call(lock_protocol::acquire, lid, id, r);
          
          pthread_mutex_lock(&m);
          if (ret == lock_protocol::OK) {
            entry->state = LOCKED;
            entry->revoked = false;
            entry->retry = false;
            pthread_mutex_unlock(&m);
            return lock_protocol::OK;
          } else if (ret == lock_protocol::RETRY) {
            // 服务器让我们稍后重试
            entry->state = NONE;
            while (!entry->retry && entry->state == NONE) {
              pthread_cond_wait(&entry->state_cv, &m);
            }
            entry->retry = false;
            continue;
          } else {
            entry->state = NONE;
            pthread_mutex_unlock(&m);
            return ret;
          }
        }
        break;
        
      case FREE:
        // 锁在本地缓存中且可用
        entry->state = LOCKED;
        pthread_mutex_unlock(&m);
        return lock_protocol::OK;
        
      case LOCKED:
      case ACQUIRING:
      case RELEASING:
        // 锁被占用或正在操作中，等待
        {
          pthread_cond_t cv;
          pthread_cond_init(&cv, NULL);
          entry->waiters.push_back(&cv);
          
          pthread_cond_wait(&cv, &m);
          
          entry->waiters.remove(&cv);
          pthread_cond_destroy(&cv);
        }
        continue;
    }
  }
  
  pthread_mutex_unlock(&m);
  return lock_protocol::OK;
}

lock_protocol::status
lock_client_cache::release(lock_protocol::lockid_t lid)
{
  pthread_mutex_lock(&m);
  
  auto it = locks.find(lid);
  if (it == locks.end()) {
    pthread_mutex_unlock(&m);
    return lock_protocol::NOENT;
  }
  
  lock_entry *entry = it->second;
  
  if (entry->state != LOCKED) {
    pthread_mutex_unlock(&m);
    return lock_protocol::NOENT;
  }
  
  if (entry->revoked) {
    // 服务器要求我们释放锁
    entry->state = RELEASING;
    pthread_mutex_unlock(&m);
    
    int r;
    lock_protocol::status ret = cl->call(lock_protocol::release, lid, id, r);
    
    pthread_mutex_lock(&m);
    if (ret == lock_protocol::OK) {
      entry->state = NONE;
      entry->revoked = false;
      
      // 唤醒等待的线程
      for (auto& cv : entry->waiters) {
        pthread_cond_signal(cv);
      }
      pthread_cond_signal(&entry->state_cv);
    }
  } else {
    // 本地释放，保持在缓存中
    entry->state = FREE;
    
    // 唤醒一个等待的线程
    if (!entry->waiters.empty()) {
      pthread_cond_signal(entry->waiters.front());
    }
  }
  
  pthread_mutex_unlock(&m);
  return lock_protocol::OK;
}

rlock_protocol::status
lock_client_cache::revoke_handler(lock_protocol::lockid_t lid, int &)
{
  pthread_mutex_lock(&m);
  
  lock_entry *entry = get_lock_entry(lid);
  entry->revoked = true;
  
  if (entry->state == FREE) {
    // 锁当前未被使用，立即释放给服务器
    entry->state = RELEASING;
    pthread_mutex_unlock(&m);
    
    int r;
    cl->call(lock_protocol::release, lid, id, r);
    
    pthread_mutex_lock(&m);
    entry->state = NONE;
    entry->revoked = false;
    pthread_cond_signal(&entry->state_cv);
  }
  
  pthread_mutex_unlock(&m);
  return rlock_protocol::OK;
}

rlock_protocol::status
lock_client_cache::retry_handler(lock_protocol::lockid_t lid, int &)
{
  pthread_mutex_lock(&m);
  
  lock_entry *entry = get_lock_entry(lid);
  entry->retry = true;
  pthread_cond_signal(&entry->state_cv);
  
  pthread_mutex_unlock(&m);
  return rlock_protocol::OK;
}