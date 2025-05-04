#include <extent_server.h>
#include <gtest/gtest.h>
#include <string.h>

TEST(SampleTest, Content) {
  auto server = extent_server();
  int tmp = 0;
  server.put(2, "data2", tmp);
  server.put(3, "data3", tmp);

  server.put(1, "file1,2;file2,3", tmp);

  std::string result;
  server.get(1, result);
  ASSERT_EQ(result, "file1,2;file2,3");

  server.remove(1, tmp);
  std::string result2;
  server.get(1, result2);
  ASSERT_EQ(result2, "");
}
struct dirent {
  std::string name;
  unsigned long inum;
};

bool operator==(const dirent &lhs, const dirent &rhs) {
  return lhs.name == rhs.name && lhs.inum == rhs.inum;
}

TEST(ServerTest, ReadDir) {

  auto tmp = 0;
  auto server = extent_server();
  server.put(1, "file1,2;file2,3;file3,4", tmp);

  std::string content;
  server.get(1, content);

  std::vector<dirent> res;

  char *token = strtok(const_cast<char *>(content.c_str()), ";");
  while (token) {
    dirent new_entry;
    std::string entry = std::string(token);
    size_t delimiter = entry.find(',');

    new_entry.name = entry.substr(0, delimiter);
    new_entry.inum = strtoul(entry.substr(delimiter + 1).c_str(), nullptr, 10);
    res.push_back(new_entry);

    token = strtok(nullptr, ";");
  }
  ASSERT_EQ(res,
            std::vector<dirent>({{"file1", 2}, {"file2", 3}, {"file3", 4}}));
}