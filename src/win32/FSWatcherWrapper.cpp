#include "../../includes/win32/FSWatcherWrapper.h"

#pragma unmanaged
FSWatcherWrapper::FSWatcherWrapper(Queue &queue, std::string path): mQueue(queue) {
  mWD = createFileWatcher(path);
}

bool FSWatcherWrapper::isWatching() {
  return mWD >= 0;
}

FSWatcherWrapper::~FSWatcherWrapper() {
  if (mWD < 0) {
    return;
  }

  destroyFileWatcher(mWD);
}
