// extent client interface.

#ifndef extent_client_cache_h
#define extent_client_cache_h

#include "extent_client.h"
#include "extent_protocol.h"
#include "lock_client_cache.h"
#include "rpc.h"
#include <map>
#include <pthread.h>
#include <string>

class cache_obj {
public:
  enum cache_state {
    None,   // not cached
    Cached, // cached
    Dirty,  // cached but dirty
    Removed // cached but removed
  };
  // should we use seperate state for metadata for better efficiency?
  cache_state state;
  std::string data;
  extent_protocol::attr attr;

  cache_obj() : state(None) {}
};

class extent_client_cache : public extent_client {
private:
  pthread_mutex_t mutex;
  std::map<extent_protocol::extentid_t, cache_obj> cache;
  // safety: mutex is held by caller
  // cache extent and metadata
  extent_protocol::status get_cache_locked(extent_protocol::extentid_t eid);
  extent_protocol::status get_cache_or_init_locked(extent_protocol::extentid_t eid);

public:
  extent_client_cache(std::string dst);

  extent_protocol::status get(extent_protocol::extentid_t eid,
                              std::string &buf);
  extent_protocol::status getattr(extent_protocol::extentid_t eid,
                                  extent_protocol::attr &a);
  extent_protocol::status setattr(extent_protocol::extentid_t eid,
                                  extent_protocol::attr a);
  extent_protocol::status put(extent_protocol::extentid_t eid, std::string buf);
  extent_protocol::status remove(extent_protocol::extentid_t eid);
  extent_protocol::status flush(extent_protocol::extentid_t eid);
};

// extent and lock share the same 'id'
class extent_client_lock_user : public lock_release_user {
private:
  extent_client_cache *ec;

public:
  extent_client_lock_user(extent_client_cache *ec) : ec(ec) {}

  void dorelease(lock_protocol::lockid_t lid) { ec->flush(lid); }
};

#endif
