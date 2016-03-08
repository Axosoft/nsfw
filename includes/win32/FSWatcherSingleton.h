#ifndef FS_WATCHER_SINGLETON_H
#define FS_WATCHER_SINGLETON_H

#include "FSWatcher.h"
#include "../Queue.h"

#using <mscorlib.dll>

using namespace System::Collections::Generic;

ref class FSWatcherSingleton {
public:
  static property FSWatcherSingleton ^Instance { FSWatcherSingleton ^get() { return %mInstance; } }
  int createFileWatcher(Queue &queue, System::String ^path);
  void destroyFileWatcher(Int32 wd);
private:
  FSWatcherSingleton();
  static FSWatcherSingleton mInstance;
  Dictionary<Int32, FSWatcher ^> ^mWatcherMap;
  int mNext;
};

int createFileWatcher(Queue &queue, std::string path);
void destroyFileWatcher(int wd);

#endif
