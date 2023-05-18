#ifndef FANOTIFY_TREE_H
#define FANOTIFY_TREE_H
#include <sys/fanotify.h>
#include <sys/stat.h>
#include <unistd.h>
#include <dirent.h>
#include <stdlib.h>
#include <fcntl.h>
#include <string>
#include <vector>
#include <functional>
#include <chrono>
#include <thread>

class FanotifyTree {
public:
  FanotifyTree(int fanotifyInstance, std::string path, const std::vector<std::string> &excludedPaths);

  bool getPath(std::string &out, int wd);
  const std::vector<std::string>& getExcludedPaths() const;
  void updateExcludedPaths(const std::vector<std::string> &excludedPaths);
  std::string getError();
  bool hasErrored();

  ~FanotifyTree();
private:
  bool existWatchedPath();

  const int mFanotifyInstance;
  std::vector<std::string> mExcludedPaths;
  std::string mPath;
  std::string mError;
};

#endif
