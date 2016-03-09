#ifndef FS_WATCHER_SINGLETON_H
#define FS_WATCHER_SINGLETON_H

#include "FSWatcher.h"

#using <mscorlib.dll>

using namespace System::Collections::Generic;

ref class FSWatcherSingleton {
public:
  static property FSWatcherSingleton ^Instance { FSWatcherSingleton ^get() { return %mInstance; } }
  int createFileWatcher(EventQueue &queue, System::String ^path);
  void destroyFileWatcher(Int32 wd);
  bool didFileWatcherError(Int32 wd);
  System::String ^getFileWatcherError(Int32 wd);
private:
  FSWatcherSingleton();
  static FSWatcherSingleton mInstance;
  Dictionary<Int32, FSWatcher ^> ^mWatcherMap;
  int mNext;
};

int createFileWatcher(EventQueue &queue, std::string path);
void destroyFileWatcher(int wd);
bool didFileWatcherError(int wd);
std::string getFileWatcherError(int wd);

#endif
