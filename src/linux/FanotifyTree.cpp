#include "../../includes/linux/FanotifyTree.h"
#include <cstdio>
#include <sys/stat.h>

#include <algorithm>

/**
 * FanotifyTree ---------------------------------------------------------------------------------------------------------
 */
FanotifyTree::FanotifyTree(int fanotifyInstance, std::string path, const std::vector<std::string> &excludedPaths):
  mFanotifyInstance(fanotifyInstance) {
  mExcludedPaths = excludedPaths;
  if (path.back() == '/') {
    mPath = path.substr(0, path.length() - 2);
  } else {
    mPath = path;
  }
}

bool FanotifyTree::getPath(std::string &out, int wd) {
  std::string fdPath = "/proc/self/fd/" + std::to_string(wd);

  #ifdef NSFW_TEST_SLOW_1
    std::this_thread::sleep_for(std::chrono::milliseconds(500));
  #endif
  
  char path[PATH_MAX];
  ssize_t len = readlink(fdPath.c_str(), path, sizeof(path) - 1);

  if (len != -1) {
    path[len] = '\0';
    out = std::string(path);
  } else {
    return false;
  }

  if (out.rfind(mPath, 0) != 0) {
    return false;
  }

  for (auto excludedPath : mExcludedPaths) {
    if (out.rfind(excludedPath, 0) == 0) {
      return false;
    }
  }
  
  return true;
}

const std::vector<std::string>& FanotifyTree::getExcludedPaths() const {
  return mExcludedPaths;
}

void FanotifyTree::updateExcludedPaths(const std::vector<std::string> &excludedPaths) {
  mExcludedPaths = excludedPaths;
}

FanotifyTree::~FanotifyTree() {
}