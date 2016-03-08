#ifndef FS_WATCHER_WRAPPER_H
#define FS_WATCHER_WRAPPER_H

#include "FSWatcherSingleton.h"

class FSWatcherWrapper {
public:
  FSWatcherWrapper(EventQueue &queue, std::string path);

  bool isWatching();

  ~FSWatcherWrapper();
private:
  int mWD;
  EventQueue &mQueue;
};

#endif
