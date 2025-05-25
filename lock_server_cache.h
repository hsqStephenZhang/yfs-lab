#ifndef lock_server_cache_h
#define lock_server_cache_h

#include <string>
#include <unordered_map>
#include <queue>
#include <utility>
#include <vector>
#include "lock_protocol.h"
#include "rpc.h"
#include "lock_server.h"
#include "pthread.h"

class lock_server_cache {
 private:
  // 锁的状态
  enum server_lock_state {
    FREE_SERVER,     // 锁空闲
    LOCKED_SERVER    // 锁被占用
  };
  
  struct server_lock_entry {
    server_lock_state state;
    std::string owner;              // 当前锁的拥有者
    std::queue<std::string> waiters; // 等待队列
    bool revoke_sent;               // 是否已发送revoke消息
    
    server_lock_entry() : state(FREE_SERVER), revoke_sent(false) {}
  };
  
  int nacquire;
  std::map<lock_protocol::lockid_t, server_lock_entry*> locks;
  pthread_mutex_t m;
  
  // 客户端连接信息
  std::map<std::string, rpcc*> clients;
  
 public:
  lock_server_cache();
  ~lock_server_cache();
  
  lock_protocol::status stat(lock_protocol::lockid_t, int &);
  lock_protocol::status acquire(lock_protocol::lockid_t, std::string id, int &);
  lock_protocol::status release(lock_protocol::lockid_t, std::string id, int &);
  
 private:
  server_lock_entry* get_server_lock_entry(lock_protocol::lockid_t lid);
  rpcc* get_client_connection(const std::string& client_id);
  void send_revoke(lock_protocol::lockid_t lid, const std::string& client_id);
  void send_retry(lock_protocol::lockid_t lid, const std::string& client_id);
};

#endif