// RPC stubs for clients to talk to extent_server

#include "extent_client_cache.h"
#include "extent_client.h"
#include "extent_protocol.h"
#include "now.h"
#include "slock.h"
#include <cassert>
#include <iostream>
#include <sstream>
#include <stdio.h>
#include <time.h>
#include <unistd.h>

// The calls assume that the caller holds a lock on the extent

extent_client_cache::extent_client_cache(std::string dst) : extent_client(dst) {
  assert(pthread_mutex_init(&mutex, NULL) == 0);
}

extent_protocol::status
extent_client_cache::get_cache_or_init_locked(extent_protocol::extentid_t eid) {
  auto ret = this->get_cache_locked(eid);
  if (ret != extent_protocol::OK) {
    // If not found, initialize the cache object
    cache_obj obj;
    obj.state = cache_obj::Cached; // Not cached
    obj.data.clear();
    obj.attr.atime = 0;
    obj.attr.mtime = 0;
    obj.attr.ctime = 0;
    obj.attr.size = 0;
    cache[eid] = obj; // Insert empty object into cache
  }
  return extent_protocol::OK;
}

extent_protocol::status
extent_client_cache::get_cache_locked(extent_protocol::extentid_t eid) {
  extent_protocol::status ret = extent_protocol::OK;
  auto it = cache.find(eid);
  // no cache or cache state is invalid
  if (it == cache.end() || it->second.state == cache_obj::None) {
    // Not in cache, fetch from server
    std::string tmp_buf;
    extent_protocol::attr tmp_attr;
    ret = cl->call(extent_protocol::get, eid, tmp_buf);
    if (ret == extent_protocol::OK) {
      ret = cl->call(extent_protocol::getattr, eid, tmp_attr);
    }
    printf("[CACHE-GETLOCKED] %llu, content:%s, ret:%d\n", eid, tmp_buf.c_str(),
           ret);
    if (ret == extent_protocol::OK) {
      // Cache the object
      cache_obj obj;
      obj.state = cache_obj::Cached;
      obj.data = tmp_buf;
      obj.attr.atime = tmp_attr.atime;
      obj.attr.mtime = tmp_attr.mtime;
      obj.attr.ctime = tmp_attr.ctime;
      obj.attr.size = tmp_attr.size;
      assert(obj.attr.size == tmp_buf.size());
      cache[eid] = obj;
    } else {
      return extent_protocol::RPCERR;
    }
  }
  return ret;
}

extent_protocol::status
extent_client_cache::get(extent_protocol::extentid_t eid, std::string &buf) {
  extent_protocol::status ret = extent_protocol::OK;
  ScopedLock l(&mutex);
  this->get_cache_locked(eid);
  auto it = cache.find(eid);
  assert(it != cache.end());
  assert(it->second.state != cache_obj::None);
  if (it->second.state == cache_obj::Removed) {
    printf("[CACHE-GET] NOENT %llu\n", eid);
    return extent_protocol::NOENT; // Object was removed
  } else {
    // Object is cached, return the cached data
    buf = it->second.data;
    it->second.attr.atime = get_now(); // Update access time
    printf("[CACHE-GET] %llu, content:%s\n", eid, buf.c_str());
    return extent_protocol::OK;
  }

  return ret;
}

extent_protocol::status
extent_client_cache::getattr(extent_protocol::extentid_t eid,
                             extent_protocol::attr &attr) {
  extent_protocol::status ret = extent_protocol::OK;
  ScopedLock l(&mutex);
  this->get_cache_locked(eid);
  auto it = cache.find(eid);
  assert(it != cache.end());
  assert(it->second.state != cache_obj::None);
  if (it->second.state == cache_obj::Removed) {
    printf("[CACHE-GETATTR] NOENT %llu\n", eid);
    return extent_protocol::NOENT; // Object was removed
  } else {
    // Object is cached, return the cached data
    attr.atime = it->second.attr.atime;
    attr.mtime = it->second.attr.mtime;
    attr.ctime = it->second.attr.ctime;
    attr.size = it->second.attr.size;
    return extent_protocol::OK;
  }

  return ret;
}

extent_protocol::status
extent_client_cache::setattr(extent_protocol::extentid_t eid,
                             extent_protocol::attr attr) {
  extent_protocol::status ret = extent_protocol::OK;
  ScopedLock l(&mutex);
  this->get_cache_locked(eid);
  auto it = cache.find(eid);
  assert(it != cache.end());
  assert(it->second.state != cache_obj::None);
  if (it->second.state == cache_obj::Removed) {
    printf("[CACHE-GET] NOENT %llu\n", eid);
    return extent_protocol::NOENT; // Object was removed
  } else {
    // Object is cached, return the cached data
    it->second.attr.atime = attr.atime;
    it->second.attr.mtime = attr.mtime;
    it->second.attr.ctime = attr.ctime;
    it->second.attr.size = attr.size;
    return extent_protocol::OK;
  }

  return ret;
}

extent_protocol::status
extent_client_cache::put(extent_protocol::extentid_t eid, std::string buf) {
  extent_protocol::status ret = extent_protocol::OK;
  ScopedLock l(&mutex);
  this->get_cache_or_init_locked(eid);
  auto it = cache.find(eid);
  printf("[CACHE-PUT] Before Putting extent %llu, mtime: %u, size: %zu\n", eid,
         it->second.attr.mtime, buf.size());
  assert(it != cache.end());
  assert(it->second.state != cache_obj::None);
  if (it->second.state == cache_obj::Removed) {
    printf("[CACHE-PUT] After NOENT %llu\n", eid);
    return extent_protocol::NOENT; // Object was removed
  } else {
    // Object is cached, update the data
    it->second.data = buf;
    it->second.state = cache_obj::Dirty; // Mark as dirty
    it->second.attr.size = buf.size();   // Update size
    auto now = get_now();
    it->second.attr.atime = now; // Update modification time
    it->second.attr.mtime = now; // Update modification time
    it->second.attr.ctime = now; // Update change time
    printf("[CACHE-PUT] After Putting extent %llu, now: %lu, mtime: %u, size: "
           "%zu\n",
           eid, now, it->second.attr.mtime, buf.size());
    return extent_protocol::OK;
  }
  return ret;
}

extent_protocol::status
extent_client_cache::remove(extent_protocol::extentid_t eid) {
  extent_protocol::status ret = extent_protocol::OK;
  ScopedLock l(&mutex);
  this->get_cache_locked(eid);
  auto it = cache.find(eid);
  assert(it != cache.end());
  assert(it->second.state != cache_obj::None);
  if (it->second.state == cache_obj::Removed) {
    return extent_protocol::NOENT; // Object was removed
  } else {
    // Object is cached, mark it as removed
    it->second.state = cache_obj::Removed; // Mark as removed
    it->second.data.clear();               // Clear the data
    it->second.attr.size = 0;              // Reset size
    auto now = get_now();
    it->second.attr.atime = now; // Update access time
    it->second.attr.mtime = now; // Update modification time
    it->second.attr.ctime = now; // Update change time
    return extent_protocol::OK;
  }
  return ret;
}

extent_protocol::status
extent_client_cache::flush(extent_protocol::extentid_t eid) {
  extent_protocol::status ret = extent_protocol::OK;
  ScopedLock l(&mutex);
  auto it = cache.find(eid);
  if (it != cache.end()) {
    if (it->second.state == cache_obj::Dirty) {
      printf("[CACHE-FLUSH] Flushing dirty extent %llu to server, state: %d\n",
             eid, it->second.state);
      // Flush the dirty object to the server
      int tmp = 0;
      ret = cl->call(extent_protocol::put, eid, it->second.data, tmp);
      if (ret == extent_protocol::OK) {
        // we lose the ownership of the obj after the flush
        ret = cl->call(extent_protocol::setattr, eid, it->second.attr, tmp);
      } else {
        printf("[CACHE-FLUSH] Failed to flush extent %llu to server, ret: %d\n",
               eid, ret);
      }
      if (ret == extent_protocol::OK) {
        printf("[CACHE-FLUSH] Flushing extent %llu to server\n", eid);
      } else {
        printf("[CACHE-FLUSH] Failed to flush extent attr %llu to server, ret: "
               "%d\n",
               eid, ret);
      }
    } else if (it->second.state == cache_obj::Removed) {
      // If the object was removed, we should also remove it from the server
      int tmp = 0;
      extent_protocol::status ret = cl->call(extent_protocol::remove, eid, tmp);
      if (ret == extent_protocol::OK) {
        printf("[CACHE-FLUSH] Flushing extent %llu (removed) to server\n", eid);
      }
    }
    cache.erase(it); // Remove from cache
  } else {
    printf("[CACHE-FLUSH] No cache for extent %llu to flush\n", eid);
    return extent_protocol::NOENT; // No cache found
  }
  return extent_protocol::OK; // Nothing to flush
}