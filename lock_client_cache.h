#ifndef lock_client_cache_h
#define lock_client_cache_h

#include "lock_protocol.h"
#include "lock_client.h"
#include "lang/verify.h"
#include <map>
#include <list>

using namespace std;

// 锁状态枚举
enum lock_state {
    NONE,       // 没有锁
    FREE,       // 有锁但未被使用
    LOCKED,     // 锁被占用
    ACQUIRING,  // 正在获取锁
    RELEASING   // 正在释放锁
};

class lock_client_cache : public lock_client {
 private:
  class lock_release_user;
  
  struct lock_entry {
    lock_state state;
    pthread_cond_t state_cv;     // 状态变化条件变量
    pthread_cond_t thread_cv;    // 线程等待条件变量
    bool revoked;                // 是否被服务器撤销
    bool retry;                  // 是否需要重试
    std::list<pthread_cond_t*> waiters;  // 等待队列
    
    lock_entry() : state(NONE), revoked(false), retry(false) {
      pthread_cond_init(&state_cv, NULL);
      pthread_cond_init(&thread_cv, NULL);
    }
    
    ~lock_entry() {
      pthread_cond_destroy(&state_cv);
      pthread_cond_destroy(&thread_cv);
    }

    void setState(lock_state new_state, std::string msg = "") {
      std::cout << "Setting state from " << state << " to " << new_state << ", msg:" << msg << std::endl;
      this->state = new_state;
    }
  };

  std::map<lock_protocol::lockid_t, lock_entry*> locks;
  pthread_mutex_t m;           // 保护locks映射的互斥锁
  
  class lock_release_user *lu;
  int rlock_port;
  std::string hostname;
  std::string id;

 public:
  static int last_port;
  
  lock_client_cache(std::string xdst, class lock_release_user *l = 0);
  virtual ~lock_client_cache();

  std::string getid() const { return id; }
  
  lock_protocol::status acquire(lock_protocol::lockid_t);
  virtual lock_protocol::status release(lock_protocol::lockid_t);
  void releaser();
  
  rlock_protocol::status revoke_handler(lock_protocol::lockid_t, 
                                        int &);
  rlock_protocol::status retry_handler(lock_protocol::lockid_t, 
                                       int &);
  
 private:
  lock_entry* get_lock_entry(lock_protocol::lockid_t lid);
  void remove_lock_entry(lock_protocol::lockid_t lid);
};

#endif