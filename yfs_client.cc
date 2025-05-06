// yfs client.  implements FS operations using extent and lock server
#include "yfs_client.h"
#include "extent_client.h"
#include <cstddef>
#include <cstring>
#include <fcntl.h>
#include <fuse/fuse_lowlevel.h>
#include <iostream>
#include <sstream>
#include <stdio.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>

yfs_client::yfs_client(std::string extent_dst, std::string lock_dst) {
  ec = new extent_client(extent_dst);
}

yfs_client::inum yfs_client::n2i(std::string n) {
  std::istringstream ist(n);
  unsigned long long finum;
  ist >> finum;
  return finum;
}

std::string yfs_client::filename(inum inum) {
  std::ostringstream ost;
  ost << inum;
  return ost.str();
}

bool yfs_client::isfile(inum inum) {
  if (inum & 0x80000000)
    return true;
  return false;
}

bool yfs_client::isdir(inum inum) { return !isfile(inum); }

int yfs_client::getattr(inum inum, fileinfo &fin) {
  int r = OK;

  printf("getattr %016llx\n", inum);
  extent_protocol::attr a;
  if (ec->getattr(inum, a) != extent_protocol::OK) {
    r = IOERR;
    goto release;
  }

  fin.atime = a.atime;
  fin.mtime = a.mtime;
  fin.ctime = a.ctime;
  fin.size = a.size;
  printf("getfile %016llx -> sz %llu\n", inum, fin.size);

release:

  return r;
}

int yfs_client::setattr(inum inum, int to_set, struct stat &st) {
  int r = OK;

  printf("setattr %016llx\n", inum);
  extent_protocol::attr a;
  a.atime = st.st_atime;
  a.mtime = st.st_mtime;
  a.ctime = st.st_ctime;
  a.size = st.st_size;
  if (ec->setattr(inum, a) != extent_protocol::OK) {
    r = IOERR;
    goto release;
  }

release:

  return r;
}

int yfs_client::getdir(inum inum, dirinfo &din) {
  int r = OK;

  printf("getdir %016llx\n", inum);
  extent_protocol::attr a;
  if (ec->getattr(inum, a) != extent_protocol::OK) {
    r = IOERR;
    goto release;
  }
  din.atime = a.atime;
  din.mtime = a.mtime;
  din.ctime = a.ctime;

release:
  return r;
}

yfs_client::status yfs_client::create(inum parent, const std::string &name,
                                      unsigned long &new_id) {
  // check if parent exists
  std::string dir_content;
  if (this->ec->get(parent, dir_content) != extent_protocol::OK)
    return yfs_client::NOENT;

  // create new empty file
  new_id = lookup(parent, name);
  if (new_id == 0) {
    this->ec->alloc_ino(0, new_id);
    new_id |= 0x80000000;
    this->ec->put(new_id, "");

    // add file to parent directory
    dir_content += name + ',' + std::to_string(new_id) + ';';
    this->ec->put(parent, dir_content);
  }

  return yfs_client::OK;
}

yfs_client::inum yfs_client::lookup(inum di, std::string name) {
  if (!isdir(di))
    return 0;

  std::string dir_content;
  if (this->ec->get(di, dir_content) != extent_protocol::OK)
    return 0;

  char *token = std::strtok(const_cast<char *>(dir_content.c_str()), ";");
  while (token) {
    std::string entry = std::string(token);
    size_t delimiter = entry.find(',');
    if (name == entry.substr(0, delimiter)) {
      return strtoul(entry.substr(delimiter + 1).c_str(), nullptr, 10);
    }
    token = std::strtok(nullptr, ";");
  }
  return 0;
}

std::vector<yfs_client::dirent> yfs_client::readdir(inum dir) {
  std::vector<dirent> res;

  std::string content;
  ec->get(dir, content);

  char *token = std::strtok(const_cast<char *>(content.c_str()), ";");
  while (token) {
    dirent new_entry;
    std::string entry = std::string(token);
    size_t delimiter = entry.find(',');

    new_entry.name = entry.substr(0, delimiter);
    new_entry.inum = strtoul(entry.substr(delimiter + 1).c_str(), nullptr, 10);
    res.push_back(new_entry);

    token = std::strtok(nullptr, ";");
  }
  std::cout << "readdir of: " << dir << "\n";
  for (const auto &entry : res) {
    std::cout << "  " << entry.name << " -> " << entry.inum << "\n";
  }
  std::cout << "end of readdir\n";

  return res;
}

// content range by get: [0, content.size())
// our range: [offset, offset + size)
yfs_client::status yfs_client::read(inum fi, size_t size, off_t offset,
                                    std::string &data) {
  if (offset < 0) {
    return yfs_client::IOERR;
  }
  if (size == 0) {
    return yfs_client::OK;
  }

  std::string content;
  if (this->ec->get(fi, content) != yfs_client::OK) {
    return yfs_client::NOENT;
  }

  if (static_cast<size_t>(offset) < content.size()) {
    data = content.substr(offset, size);
  } else if (static_cast<size_t>(offset) >= content.size()) {
    data = std::string();
  }

  return yfs_client::OK;
}

// content range by put: [0, old_content.size())
// our range: [offset, offset + data.size())
yfs_client::status yfs_client::write(inum fi, std::string &data, off_t offset) {
  if (offset < 0) {
    return yfs_client::IOERR;
  }

  std::string old_content;
  if (this->ec->get(fi, old_content) != extent_protocol::OK)
    return yfs_client::NOENT;

  if (static_cast<size_t>(offset) <= old_content.size()) {
    if (offset + data.size() <= old_content.size()) {
      old_content.replace(offset, data.size(), data);
    } else {
      old_content.replace(offset, old_content.size() - offset, data);
      old_content +=
          std::string(offset + data.size() - old_content.size(), '\0');
    }
  } else {
    old_content.resize(offset, '\0');
    old_content += data;
  }

  this->ec->put(fi, old_content);

  return yfs_client::OK;
}
