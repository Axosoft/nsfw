#include "../../includes/osx/FSEventsBarrier.h"
#include "../../includes/FilterPaths.h"

#include <limits.h>
#include <stdlib.h>
#include <unistd.h>
#include <mutex>
#include <vector>

namespace {

const char BARRIER_PREFIX[] = ".nsfw-barrier.";

// The real path of the user's temporary directory, as FSEvents reports it, with a trailing slash; empty if unknown
// yet. Looked up until found.
std::string temporaryDirectory() {
  static std::mutex mutex;
  static std::string directory;
  std::lock_guard<std::mutex> lock(mutex);
  if (directory.empty()) {
    char temporary[PATH_MAX];
    const size_t length = confstr(_CS_DARWIN_USER_TEMP_DIR, temporary, sizeof(temporary));
    char real[PATH_MAX];
    if (length > 0 && length <= sizeof(temporary) && realpath(temporary, real) != NULL) {
      directory = real;
      if (directory.empty() || directory.back() != '/') directory += '/';
    }
  }
  return directory;
}

}

std::string createFSEventsBarrierDirectory() {
  const std::string temporary = temporaryDirectory();
  if (temporary.empty()) return std::string();
  const std::string pattern = temporary + BARRIER_PREFIX + "XXXXXX";
  std::vector<char> directory(pattern.begin(), pattern.end());
  directory.push_back('\0');
  return mkdtemp(directory.data()) == NULL ? std::string() : std::string(directory.data());
}

bool isFSEventsBarrierPath(const std::string &path) {
  // Not looked up for paths that can't be one.
  if (path.find(BARRIER_PREFIX) == std::string::npos) return false;
  const std::string temporary = temporaryDirectory();
  return !temporary.empty() && filterpaths::startsWith(path, temporary + BARRIER_PREFIX, true);
}
