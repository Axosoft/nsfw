#include "../../includes/win32/FSWatcherSingleton.h"

#pragma managed
FSWatcherSingleton::FSWatcherSingleton() {
  mWatcherMap = gcnew Dictionary<Int32, FSWatcher^>();
  mNext = 0;
}

int FSWatcherSingleton::createFileWatcher(EventQueue &queue, System::String ^path) {
  try {
    FSWatcher ^watcher = gcnew FSWatcher(queue, path);
    int wd = mNext;
    mNext = mNext == Int32::MaxValue ? 0 : mNext + 1;
    mWatcherMap[wd] = watcher;
    return wd;
  } catch (System::Exception ^e) {
    // TODO: return this error to javascript
    return -1;
  }
}

void FSWatcherSingleton::destroyFileWatcher(Int32 wd) {
  if (!mWatcherMap->ContainsKey(wd)) {
    return;
  }

  FSWatcher ^watcher = mWatcherMap[wd];
  mWatcherMap->Remove(wd);
  delete watcher;
}

int createFileWatcher(EventQueue &queue, std::string path) {
  return FSWatcherSingleton::Instance->createFileWatcher(queue, gcnew System::String(path.c_str()));
}

void destroyFileWatcher(int wd) {
  FSWatcherSingleton::Instance->destroyFileWatcher(wd);
}
