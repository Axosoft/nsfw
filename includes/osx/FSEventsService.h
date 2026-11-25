#ifndef NSFW_FS_EVENTS_SERVICE_H
#define NSFW_FS_EVENTS_SERVICE_H

#include "RunLoop.h"
#include "../Queue.h"

#include <CoreServices/CoreServices.h>
#include <time.h>
#include <sys/stat.h>
#include <map>
#include <mutex>
#include <vector>
#include <string>

void FSEventsServiceCallback(
  ConstFSEventStreamRef streamRef,
  void *clientCallBackInfo,
  size_t numEvents,
  void *eventPaths,
  const FSEventStreamEventFlags eventFlags[],
  const FSEventStreamEventId eventIds[]
);

class RunLoop;

class FSEventsService {
public:
  FSEventsService(std::shared_ptr<EventQueue> queue, std::string path, const std::vector<std::string> &excludedPaths,
    bool recursive = true);

  friend void FSEventsServiceCallback(
    ConstFSEventStreamRef streamRef,
    void *clientCallBackInfo,
    size_t numEvents,
    void *eventPaths,
    const FSEventStreamEventFlags eventFlags[],
    const FSEventStreamEventId eventIds[]
  );
  std::string getError();
  bool hasErrored();
  bool isWatching();
  // The run loop thread ended: on its own it can't report anything anymore, which hasErrored() tells.
  void runLoopExited();
  void updateExcludedPaths(const std::vector<std::string> &excludedPaths);

  ~FSEventsService();
private:
  void create(std::string path);
  void demangle(std::string path, PathFilter::Kind kind = PathFilter::Kind::UNKNOWN);
  void dispatch(EventType action, std::string path, PathFilter::Kind kind = PathFilter::Kind::UNKNOWN);
  void modify(std::string path);
  void remove(std::string path, PathFilter::Kind kind = PathFilter::Kind::UNKNOWN);
  void rename(std::vector<std::string> &paths, const std::map<std::string, PathFilter::Kind> &kinds);
  void splitFilePath(std::string &directory, std::string &name, const std::string &path);
  bool isExcluded(std::string filePath);
  bool isOutsideChildren(const char *filePath) const;
  std::vector<CFStringRef> createExcludedPaths(const std::vector<std::string> &excludedPaths) const;
  bool excludesRoot(const std::vector<CFStringRef> &excludedPaths) const;
  static void releaseExcludedPaths(const std::vector<CFStringRef> &excludedPaths);

  std::string mPath;
  RunLoop *mRunLoop;
  std::shared_ptr<EventQueue> mQueue;
  // Read on the FSEvents run loop thread, replaced from a worker thread by updateExcludedPaths.
  std::mutex mExcludedPathsLock;
  std::vector<CFStringRef> mExcludedPaths;
  bool mRootExcluded;
  bool mRootChanged;
  bool mCaseSensitive;
  bool mRecursive;
};

#endif
