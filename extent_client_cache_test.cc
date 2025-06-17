#include "extent_client_cache.h"
#include "extent_server.h"
#include "lock_client_cache.h"
#include "lock_server_cache.h"
#include "rpc.h"
#include <arpa/inet.h>
#include <gtest/gtest.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

// TEST(SimpleTest, interleaving) {
//   rpcs server(6666, 0);
//   extent_server es = extent_server();

//   server.reg(extent_protocol::get, &es, &extent_server::get);
//   server.reg(extent_protocol::getattr, &es, &extent_server::getattr);
//   server.reg(extent_protocol::setattr, &es, &extent_server::setattr);
//   server.reg(extent_protocol::put, &es, &extent_server::put);
//   server.reg(extent_protocol::remove, &es, &extent_server::remove);
//   server.reg(extent_protocol::alloc_ino, &es, &extent_server::alloc_ino);

//   rpcs server2(6667, 0);
//   lock_server_cache ls;
//   server2.reg(lock_protocol::stat, &ls, &lock_server_cache::stat);
//   server2.reg(lock_protocol::acquire, &ls, &lock_server_cache::acquire);
//   server2.reg(lock_protocol::release, &ls, &lock_server_cache::release);

//   lock_client_cache **lc = new lock_client_cache *[2];
//   extent_client **ec = new extent_client *[2];

//   extent_client_cache *new_ec1 = new extent_client_cache("6666");
//   lock_release_user *lu1 = new extent_client_lock_user(new_ec1);
//   lc[0] = new lock_client_cache("6667", lu1);
//   ec[0] = new_ec1;

//   extent_client_cache *new_ec2 = new extent_client_cache("6666");
//   lock_release_user *lu2 = new extent_client_lock_user(new_ec2);
//   lc[1] = new lock_client_cache("6667", lu2);
//   ec[1] = new_ec2;

//   std::string buf;

//   printf("step1\n");
//   lc[0]->acquire(1);
//   ec[0]->put(1, "a,1");
//   lc[0]->release(1);

//   printf("step2\n");
//   lc[1]->acquire(1);
//   ec[1]->get(1, buf);
//   EXPECT_EQ(buf, "a,1");
//   lc[1]->release(1);

//   printf("step3\n");
//   lc[1]->acquire(1);
//   ec[1]->put(1, "a,1;b,2;c,3");
//   ec[1]->get(1, buf);
//   EXPECT_EQ(buf, "a,1;b,2;c,3");
//   lc[1]->release(1);

//   printf("step4\n");
//   lc[0]->acquire(1);
//   ec[0]->get(1, buf);
//   EXPECT_EQ(buf, "a,1;b,2;c,3");
//   ec[0]->put(1, "a,1;b,2;c,3;d,4");
//   lc[0]->release(1);

//   printf("step5\n");
//   lc[1]->acquire(1);
//   ec[1]->get(1, buf);
//   EXPECT_EQ(buf, "a,1;b,2;c,3;d,4");
//   lc[1]->release(1);

//   printf("step6\n");
//   lc[0]->acquire(1);
//   ec[0]->remove(1);
//   lc[0]->release(1);

//   printf("step7\n");
//   lc[1]->acquire(1);
//   auto ret = ec[1]->get(1, buf);
//   EXPECT_EQ(ret, extent_protocol::NOENT);
//   lc[1]->release(1);
// }

TEST(SimpleTest, realworld) {
  rpcs server(5666, 0);
  extent_server es = extent_server();

  server.reg(extent_protocol::get, &es, &extent_server::get);
  server.reg(extent_protocol::getattr, &es, &extent_server::getattr);
  server.reg(extent_protocol::setattr, &es, &extent_server::setattr);
  server.reg(extent_protocol::put, &es, &extent_server::put);
  server.reg(extent_protocol::remove, &es, &extent_server::remove);
  server.reg(extent_protocol::alloc_ino, &es, &extent_server::alloc_ino);

  rpcs server2(5667, 0);
  lock_server_cache ls;
  server2.reg(lock_protocol::stat, &ls, &lock_server_cache::stat);
  server2.reg(lock_protocol::acquire, &ls, &lock_server_cache::acquire);
  server2.reg(lock_protocol::release, &ls, &lock_server_cache::release);

  lock_client_cache **lc = new lock_client_cache *[2];
  extent_client **ec = new extent_client *[2];

  extent_client_cache *new_ec1 = new extent_client_cache("5666");
  lock_release_user *lu1 = new extent_client_lock_user(new_ec1);
  lc[0] = new lock_client_cache("5667", lu1);
  ec[0] = new_ec1;

  extent_client_cache *new_ec2 = new extent_client_cache("5666");
  lock_release_user *lu2 = new extent_client_lock_user(new_ec2);
  lc[1] = new lock_client_cache("5667", lu2);
  ec[1] = new_ec2;

  int inode_root = 1;
  int inode_file_a = 2 & 0x80000000;

  std::string buf;
  lc[0]->acquire(inode_root);
  ec[0]->get(inode_root, buf);
  ASSERT_EQ(buf, "");
  // new file:a
  lc[0]->acquire(inode_file_a);
  ec[0]->put(inode_root, "a,1");
  ec[0]->put(inode_file_a, "content of file a");
  lc[0]->release(inode_file_a);
  lc[0]->release(inode_root);

  lc[1]->acquire(inode_root);
  ec[1]->get(inode_root, buf);
  ASSERT_EQ(buf, "a,1");
  // simulator WRONG ACCESS, the cache is accessed before the lock is acquired
  auto ret = ec[1]->get(inode_file_a, buf);
  ASSERT_EQ(ret, extent_protocol::NOENT);
  lc[1]->acquire(inode_file_a);
  ret = ec[1]->get(inode_file_a, buf);
  ASSERT_EQ(ret, extent_protocol::OK);
  ASSERT_EQ(buf, "content of file a");
  lc[1]->release(inode_file_a);
  lc[1]->release(inode_root);

  // modify file a, should be cached in client 0
  lc[0]->acquire(inode_file_a);
  ec[0]->get(inode_file_a, buf);
  ASSERT_EQ(buf, "content of file a");
  ec[0]->put(inode_file_a, "content of file a, modified0");
  ec[0]->get(inode_file_a, buf);
  ASSERT_EQ(buf, "content of file a, modified0");
  ec[0]->put(inode_file_a, "content of file a, modified1");
  ec[0]->get(inode_file_a, buf);
  ASSERT_EQ(buf, "content of file a, modified1");

  lc[0]->release(inode_file_a);
}