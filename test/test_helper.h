#include <cstdio>
#include <fstream>
#include <iostream>

class DummyFile {
  std::string path;

public:
  DummyFile(const std::string &path) : path(path) {
    std::ofstream(path.c_str());
  }

  void rename(const std::string &newPath) {
    std::rename(path.c_str(), newPath.c_str());
    path = newPath;
  }

  void modify(const std::string &payload) {
    std::ofstream ofs(path.c_str());
    ofs << payload;
  }

  ~DummyFile() { std::remove(path.c_str()); }
};
