// the extent server implementation

#include "extent_server.h"
#include "extent_protocol.h"
#include "slock.h"
#include <cassert>
#include <fcntl.h>
#include <pthread.h>
#include <sstream>
#include <stdio.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>

int extent_server::put(extent_protocol::extentid_t id, std::string buf,
                       int &r) {
  ScopedLock l(&m);
  printf("[put] id: %016llx\n", id);

  extent_protocol::attr a;
  auto now = time(0);
  a.atime = a.ctime = a.mtime = now;
  a.size = buf.size();
  if (this->store.find(id) != this->store.end()) {
    a.atime = this->store[id].attr.atime;
    this->store[id].attr = a;
    this->store[id].data = buf;
  } else {
    Item new_item = {
        .data = buf,
        .attr = a,
    };
    this->store[id] = new_item;
  }

  return extent_protocol::OK;
}

int extent_server::get(extent_protocol::extentid_t id, std::string &buf) {
  ScopedLock l(&m);
  printf("[get] id: %016llx\n", id);

  if (this->store.find(id) != this->store.end()) {
    buf = this->store[id].data;
    this->store[id].attr.atime = time(0);
    printf("[get] data: %s\n", buf.c_str());
    return extent_protocol::OK;
  } else {
    return extent_protocol::NOENT;
  }
}

int extent_server::getattr(extent_protocol::extentid_t id,
                           extent_protocol::attr &a) {
  ScopedLock l(&m);

  if (this->store.find(id) == this->store.end()) {
    return extent_protocol::NOENT;
  } else {
    a = store[id].attr;

    return extent_protocol::OK;
  }
}

// we use the same attr defind in FUSE
#define FUSE_SET_ATTR_SIZE (1 << 3)
#define FUSE_SET_ATTR_ATIME (1 << 4)
#define FUSE_SET_ATTR_MTIME (1 << 5)

int extent_server::setattr(extent_protocol::extentid_t id,
                           extent_protocol::attr a, int &r) {
  ScopedLock l(&m);

  if (this->store.find(id) == this->store.end()) {
    return extent_protocol::NOENT;
  } else {
    // TODO: shall we check?
    resize_item_data(id, a.size);
    store[id].attr.size = a.size;
    store[id].attr.atime = a.atime;
    store[id].attr.mtime = a.mtime;
    store[id].attr.ctime = time(0);

    return extent_protocol::OK;
  }
}

int extent_server::remove(extent_protocol::extentid_t id, int &r) {
  ScopedLock l(&m);
  printf("[remove] id: %016llx\n", id);
  this->store.erase(id);

  r = extent_protocol::OK;
  return extent_protocol::OK;
}

int extent_server::alloc_ino(extent_protocol::extentid_t id,
                             unsigned long &ino) {
  ScopedLock l(&m);
  ino = next_id;
  next_id++;

  return extent_protocol::OK;
}

// safety: hold the lock and item exists
void extent_server::resize_item_data(extent_protocol::extentid_t id,
                                     unsigned long new_size) {
  auto it = this->store.find(id);
  auto &item = it->second;
  if (new_size > item.data.size()) {
    item.data.resize(new_size);
  } else {
    item.data = item.data.substr(0, new_size);
  }
}