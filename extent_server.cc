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
  // You replace this with a real implementation. We send a phony response
  // for now because it's difficult to get FUSE to do anything (including
  // unmount) if getattr fails.
  ScopedLock l(&m);

  if (this->store.find(id) == this->store.end()) {
    return extent_protocol::NOENT;
  } else {
    a = store[id].attr;

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

int extent_server::alloc_ino(extent_protocol::extentid_t id, unsigned long &ino) {
  ScopedLock l(&m);
  ino = next_id;
  next_id++;

  return extent_protocol::OK;
}