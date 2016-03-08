#ifndef FS_WATCHER_WRAPPER_H
#define FS_WATCHER_WRAPPER_H

#include "FSWatcherSingleton.h"
#include "../Queue.h"

class FSWatcherWrapper {
public:
  FSWatcherWrapper(Queue &queue, std::string path);

  bool isWatching();

  ~FSWatcherWrapper();
private:
  int mWD;
};

#endif
