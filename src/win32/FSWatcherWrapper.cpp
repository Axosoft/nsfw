#include "../../includes/win32/FSWatcherWrapper.h"

#pragma unmanaged
FSWatcherWrapper::FSWatcherWrapper(EventQueue &queue, std::string path): mQueue(queue) {
  mWD = createFileWatcher(queue, path);
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
