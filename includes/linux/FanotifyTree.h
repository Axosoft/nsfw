#ifndef FANOTIFY_TREE_H
#define FANOTIFY_TREE_H
#include <sys/fanotify.h>
#include <unistd.h>
#include <dirent.h>
#include <stdlib.h>
#include <fcntl.h>
#include <string>
#include <vector>
#include <functional>

class FanotifyTree {
public:
  FanotifyTree(int fanotifyInstance, std::string path, const std::vector<std::string> &excludedPaths);

  bool getPath(std::string &out, int wd);
  const std::vector<std::string>& getExcludedPaths() const;
  void updateExcludedPaths(const std::vector<std::string> &excludedPaths);

  ~FanotifyTree();
private:
  const int mFanotifyInstance;
  std::vector<std::string> mExcludedPaths;
  std::string mPath;
};

#endif
