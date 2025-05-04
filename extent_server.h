// this is the extent server

#ifndef extent_server_h
#define extent_server_h

#include "extent_protocol.h"
#include <atomic>
#include <cstddef>
#include <ctime> // for time()
#include <map>
#include <pthread.h>
#include <string>
#include <unordered_map>
#include <utils.h>

// will be a simple K,V in-memory store in lab2
class extent_server {

  class Item {
  public:
    std::string data;
    extent_protocol::attr attr;
  };

private:
  pthread_mutex_t m;
  std::unordered_map<extent_protocol::extentid_t, Item> store;
  unsigned long next_id;

public:
  extent_server() {
    DEBUG_ASSERT_EQ(pthread_mutex_init(&m, 0), 0);
    auto tmp = 0;
    // create root node
    this->put(1, "", tmp);
    next_id = 2;
  }
  ~extent_server() { DEBUG_ASSERT_EQ(pthread_mutex_destroy(&m), 0); }

  // we do not care about the size & offset in lab2
  int put(extent_protocol::extentid_t id, std::string, int &);
  int get(extent_protocol::extentid_t id, std::string &);
  int getattr(extent_protocol::extentid_t id, extent_protocol::attr &);
  int remove(extent_protocol::extentid_t id, int &);
  int alloc_ino(extent_protocol::extentid_t id, unsigned long &ino);
};

#endif
