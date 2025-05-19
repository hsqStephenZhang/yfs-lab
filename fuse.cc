/*
 * receive request from fuse and call methods of yfs_client
 *
 * started life as low-level example in the fuse distribution.  we
 * have to use low-level interface in order to get i-numbers.  the
 * high-level interface only gives us complete paths.
 */

#include "yfs_client.h"
#include <algorithm>
#include <arpa/inet.h>
#include <assert.h>
#include <errno.h>
#include <fcntl.h>
#include <fuse/fuse_lowlevel.h>
#include <fuse_lowlevel.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <unistd.h>

int myid;
yfs_client *yfs;

int id() { return myid; }

yfs_client::status getattr_helper(yfs_client::inum inum, struct stat &st) {
  yfs_client::status ret;

  bzero(&st, sizeof(st));

  st.st_ino = inum;
  printf("getattr_helper %016llx %d\n", inum, yfs->isfile(inum));
  if (yfs->isfile(inum)) {
    yfs_client::fileinfo info;
    ret = yfs->getattr(inum, info);
    if (ret != yfs_client::OK)
      return ret;
    st.st_mode = S_IFREG | 0666;
    st.st_nlink = 1;
    st.st_atime = info.atime;
    st.st_mtime = info.mtime;
    st.st_ctime = info.ctime;
    st.st_size = info.size;
    printf("   getattr -> %llu\n", info.size);
  } else {
    yfs_client::dirinfo info;
    ret = yfs->getdir(inum, info);
    if (ret != yfs_client::OK)
      return ret;
    st.st_mode = S_IFDIR | 0777;
    st.st_nlink = 2;
    st.st_atime = info.atime;
    st.st_mtime = info.mtime;
    st.st_ctime = info.ctime;
    printf("   getattr -> %lu %lu %lu\n", info.atime, info.mtime, info.ctime);
  }
  return yfs_client::OK;
}

// create a file in the directory referred to by parent
// if the parent directory is not found, return NOENT
// if the file is created successfully, will fill the inode number in e and
// return OK
yfs_client::status create_helper(fuse_ino_t parent, const char *name,
                                 mode_t mode, struct fuse_entry_param *e,
                                 bool is_file) {

  if (!yfs->isdir(parent)) {
    return yfs_client::NOENT;
  }
  unsigned long new_ino;
  // ignore the mode here
  auto res = yfs->create(parent, name, new_ino, is_file);

  if (res != yfs_client::OK) {
    return res;
  }

  e->ino = new_ino;
  getattr_helper(new_ino, e->attr);
  printf("create_helper %s %016lx %ld\n", name, new_ino, e->attr.st_mtime);

  return yfs_client::OK;
}

void fuseserver_getattr(fuse_req_t req, fuse_ino_t ino,
                        struct fuse_file_info *fi) {
  struct stat st;
  yfs_client::inum inum = ino; // req->in.h.nodeid;
  yfs_client::status ret;

  ret = getattr_helper(inum, st);
  if (ret != yfs_client::OK) {
    fuse_reply_err(req, ENOENT);
    return;
  }
  fuse_reply_attr(req, &st, 0);
}

void fuseserver_setattr(fuse_req_t req, fuse_ino_t ino, struct stat *attr,
                        int to_set, struct fuse_file_info *fi) {
  printf("fuseserver_setattr %ld", ino);
  yfs_client::inum inum = ino; // req->in.h.nodeid;
  struct stat st;
  yfs_client::status ret;
  if (getattr_helper(inum, st) != yfs_client::OK) {
    fuse_reply_err(req, ENOENT);
    return;
  }
  printf("fuse before setattr: %016lx, size: %ld, atime: %ld, mtime: %ld\n",
         ino, st.st_size, st.st_atime, st.st_mtime);
  if (FUSE_SET_ATTR_SIZE & to_set) {
    st.st_size = attr->st_size;
    auto current_time = std::max(time(0), st.st_mtime) + 1;
    st.st_atime = current_time;
    st.st_mtime = current_time;
    st.st_ctime = current_time;
    printf("   fuseserver_setattr set size to %zu, current time: %lu, time to "
           "%lu\n",
           attr->st_size, current_time, st.st_mtime);
  }
  if (FUSE_SET_ATTR_ATIME & to_set) {
    printf("   fuseserveyr_setattr set atime to %zu\n", attr->st_atime);
    st.st_atime = attr->st_atime;
  }
  if (FUSE_SET_ATTR_MTIME & to_set) {
    printf("   fuseserver_setattr set mtime to %zu\n", attr->st_mtime);
    st.st_mtime = attr->st_mtime;
  }
  ret = yfs->setattr(inum, st);

  {
    struct stat st;
    if (getattr_helper(inum, st) != yfs_client::OK) {
      fuse_reply_err(req, ENOENT);
      return;
    }
    printf("fuse after setattr: %016lx, size: %ld, atime: %ld, mtime: %ld\n",
           ino, st.st_size, st.st_atime, st.st_mtime);
  }
  if (ret != yfs_client::OK) {
    fuse_reply_err(req, ENOENT);
    return;
  }
  fuse_reply_attr(req, &st, 0);
}

void fuseserver_read(fuse_req_t req, fuse_ino_t ino, size_t size, off_t off,
                     struct fuse_file_info *fi) {
  if (off >= 0) {
    std::string data;
    if (yfs->read(ino, size, off, data) == yfs_client::OK) {
      fuse_reply_buf(req, data.c_str(), size);
      return;
    }
  }
  fuse_reply_err(req, ENOSYS);
}

void fuseserver_write(fuse_req_t req, fuse_ino_t ino, const char *buf,
                      size_t size, off_t off, struct fuse_file_info *fi) {
  if (off >= 0) {
    auto data = std::string(buf, size);
    if (yfs->write(ino, data, off) == yfs_client::OK) {
      fuse_reply_write(req, size);
      return;
    }
  }
  fuse_reply_err(req, ENOSYS);
}

void fuseserver_create(fuse_req_t req, fuse_ino_t parent, const char *name,
                       mode_t mode, struct fuse_file_info *fi) {
  struct fuse_entry_param e;
  if (create_helper(parent, name, mode, &e, true) == yfs_client::OK) {
    fuse_reply_create(req, &e, fi);
  } else {
    fuse_reply_err(req, ENOENT);
  }
}

void fuseserver_mknod(fuse_req_t req, fuse_ino_t parent, const char *name,
                      mode_t mode, dev_t rdev) {
  struct fuse_entry_param e;
  if (create_helper(parent, name, mode, &e, true) == yfs_client::OK) {
    fuse_reply_entry(req, &e);
  } else {
    fuse_reply_err(req, ENOENT);
  }
}

void fuseserver_lookup(fuse_req_t req, fuse_ino_t parent, const char *name) {
  struct fuse_entry_param e;
  e.attr_timeout = 0.0;
  e.entry_timeout = 0.0;

  // You fill this in:
  // Look up the file named `name' in the directory referred to by
  // `parent' in YFS. If the file was found, initialize e.ino and
  // e.attr appropriately.
  auto inum = yfs->lookup(parent, name);
  if (inum > 0) {
    auto res = getattr_helper(inum, e.attr);
    if (res != yfs_client::OK) {
      fuse_reply_err(req, ENOENT);
      return;
    }
    e.ino = inum;
    fuse_reply_entry(req, &e);
  } else {
    fuse_reply_err(req, ENOENT);
  }
}

struct dirbuf {
  char *p;
  size_t size;
};

void dirbuf_add(struct dirbuf *b, const char *name, fuse_ino_t ino) {
  struct stat stbuf;
  size_t oldsize = b->size;
  b->size += fuse_dirent_size(strlen(name));
  b->p = (char *)realloc(b->p, b->size);
  memset(&stbuf, 0, sizeof(stbuf));
  stbuf.st_ino = ino;
  fuse_add_dirent(b->p + oldsize, name, &stbuf, b->size);
}

#define min(x, y) ((x) < (y) ? (x) : (y))

int reply_buf_limited(fuse_req_t req, const char *buf, size_t bufsize,
          off_t off, size_t maxsize)
{
  if ((size_t)off < bufsize)
    return fuse_reply_buf(req, buf + off, min(bufsize - off, maxsize));
  else
    return fuse_reply_buf(req, NULL, 0);
}

void fuseserver_readdir(fuse_req_t req, fuse_ino_t ino, size_t size, off_t off,
                        struct fuse_file_info *fi) {
  yfs_client::inum inum = ino; // req->in.h.nodeid;
  struct dirbuf b;
  yfs_client::dirent e;

  printf("fuseserver_readdir\n");

  if (!yfs->isdir(inum)) {
    fuse_reply_err(req, ENOTDIR);
    return;
  }

  memset(&b, 0, sizeof(b));

  // fill in the b data structure using dirbuf_add
  auto dirs = yfs->readdir(ino);
  for (const yfs_client::dirent &entry : dirs) {
    dirbuf_add(&b, entry.name.c_str(), entry.inum);
  }

  reply_buf_limited(req, b.p, b.size, off, size);
  free(b.p);
}

void fuseserver_open(fuse_req_t req, fuse_ino_t ino,
                     struct fuse_file_info *fi) {
  printf("open ino: %016lx, flags: %u\n", ino, fi->flags);
  if (fi->flags & (O_CREAT | O_WRONLY | O_RDWR)) {
    struct stat st;
    yfs_client::status ret = getattr_helper(ino, st);
    if (ret != yfs_client::OK) {
      fuse_reply_err(req, ENOENT);
      return;
    }
    auto orig_mtime = st.st_mtime;
    st.st_mtime = time(0) + 1;
    yfs->setattr(ino, st);

    struct stat new_st;
    yfs_client::status ret2 = getattr_helper(ino, new_st);
    if (ret2 != yfs_client::OK) {
      fuse_reply_err(req, ENOENT);
      return;
    }

    printf("update mtime %016lx, orig mtime: %ld, new mtime: %ld\n", ino,
           orig_mtime, new_st.st_mtime);
  }
  fuse_reply_open(req, fi);
}

void fuseserver_mkdir(fuse_req_t req, fuse_ino_t parent, const char *name,
                      mode_t mode) {
  if (!yfs->isdir(parent)) {
    fuse_reply_err(req, ENOTDIR);
    return;
  }

  struct fuse_entry_param e;
  if (create_helper(parent, name, mode, &e, false) == yfs_client::OK) {
    fuse_reply_entry(req, &e);
  } else {
    fuse_reply_err(req, ENOSYS);
  }
}

void fuseserver_unlink(fuse_req_t req, fuse_ino_t parent, const char *name) {

  // You fill this in
  // Success:	fuse_reply_err(req, 0);
  // Not found:	fuse_reply_err(req, ENOENT);
  if (!yfs->isdir(parent)) {
    fuse_reply_err(req, ENOTDIR);
    return;
  }
  auto res = yfs->unlink(parent, name);
  if (res == yfs_client::OK) {
    fuse_reply_err(req, 0);
    return;
  } else if (res == yfs_client::NOENT) {
    fuse_reply_err(req, ENOENT);
    return;
  } else {
    fuse_reply_err(req, ENOSYS);
    return;
  }
}

void fuseserver_statfs(fuse_req_t req) {
  struct statvfs buf;

  printf("statfs\n");

  memset(&buf, 0, sizeof(buf));

  buf.f_namemax = 255;
  buf.f_bsize = 512;

  fuse_reply_statfs(req, &buf);
}

struct fuse_lowlevel_ops fuseserver_oper;

int main(int argc, char *argv[]) {
  char *mountpoint = 0;
  int err = -1;
  int fd;

  setvbuf(stdout, NULL, _IONBF, 0);

  if (argc != 4) {
    fprintf(stderr, "Usage: yfs_client <mountpoint> <port-extent-server> "
                    "<port-lock-server>\n");
    exit(1);
  }
  mountpoint = argv[1];

  srandom(getpid());

  myid = random();

  yfs = new yfs_client(argv[2], argv[3]);

  fuseserver_oper.getattr = fuseserver_getattr;
  fuseserver_oper.statfs = fuseserver_statfs;
  fuseserver_oper.readdir = fuseserver_readdir;
  fuseserver_oper.lookup = fuseserver_lookup;
  fuseserver_oper.create = fuseserver_create;
  fuseserver_oper.mknod = fuseserver_mknod;
  fuseserver_oper.open = fuseserver_open;
  fuseserver_oper.read = fuseserver_read;
  fuseserver_oper.write = fuseserver_write;
  fuseserver_oper.setattr = fuseserver_setattr;
  fuseserver_oper.unlink = fuseserver_unlink;
  fuseserver_oper.mkdir = fuseserver_mkdir;

  const char *fuse_argv[20];
  int fuse_argc = 0;
  fuse_argv[fuse_argc++] = argv[0];
#ifdef __APPLE__
  fuse_argv[fuse_argc++] = "-o";
  fuse_argv[fuse_argc++] = "nolocalcaches"; // no dir entry caching
  fuse_argv[fuse_argc++] = "-o";
  fuse_argv[fuse_argc++] = "daemon_timeout=86400";
#endif

  // everyone can play, why not?
  // fuse_argv[fuse_argc++] = "-o";
  // fuse_argv[fuse_argc++] = "allow_other";

  fuse_argv[fuse_argc++] = mountpoint;
  fuse_argv[fuse_argc++] = "-d";

  fuse_args args = FUSE_ARGS_INIT(fuse_argc, (char **)fuse_argv);
  int foreground;
  int res =
      fuse_parse_cmdline(&args, &mountpoint, 0 /*multithreaded*/, &foreground);
  if (res == -1) {
    fprintf(stderr, "fuse_parse_cmdline failed\n");
    return 0;
  }

  args.allocated = 0;

  fd = fuse_mount(mountpoint, &args);
  if (fd == -1) {
    fprintf(stderr, "fuse_mount failed\n");
    exit(1);
  }

  struct fuse_session *se;

  se =
      fuse_lowlevel_new(&args, &fuseserver_oper, sizeof(fuseserver_oper), NULL);
  if (se == 0) {
    fprintf(stderr, "fuse_lowlevel_new failed\n");
    exit(1);
  }

  struct fuse_chan *ch = fuse_kern_chan_new(fd);
  if (ch == NULL) {
    fprintf(stderr, "fuse_kern_chan_new failed\n");
    exit(1);
  }

  fuse_session_add_chan(se, ch);
  // err = fuse_session_loop_mt(se);   // FK: wheelfs does this; why?
  err = fuse_session_loop(se);

  fuse_session_destroy(se);
  close(fd);
  fuse_unmount(mountpoint);

  return err ? 1 : 0;
}
