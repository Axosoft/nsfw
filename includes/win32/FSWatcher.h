#ifndef FS_WATCHER_H
#define FS_WATCHER_H

#include "../Queue.h"
#include <string>
#include <msclr\marshal_cppstd.h>

#using <mscorlib.dll>
#using <system.dll>

using namespace System;
using namespace System::IO;
using namespace System::Runtime::InteropServices;
using namespace System::Collections::Generic;
using namespace System::Collections::Concurrent;
using namespace System::Threading;

ref class FSWatcher {
public:
  FSWatcher(EventQueue &queue, System::String ^path);
  ~FSWatcher();
  void dispatchSetup();
  void eventHandlerHelper(EventType action, FileSystemEventArgs ^e);
  void onChangedDispatch(Object ^source, FileSystemEventArgs ^e);
  void onCreatedDispatch(Object ^source, FileSystemEventArgs ^e);
  void onDeletedDispatch(Object ^source, FileSystemEventArgs ^e);
  void onError(ErrorEventArgs ^e);
  void onErrorDispatch(Object ^source, ErrorEventArgs ^e);
  void onRenamed(RenamedEventArgs ^e);
  void onRenamedDispatch(Object ^source, RenamedEventArgs ^e);
private:
  ConcurrentQueue< Tuple<Int32, Object ^> ^> ^mDispatchQueue;
  bool mDispatching;
  bool mDispatchExit;
  FileSystemWatcher ^mWatcher;
  EventQueue &mQueue;
};

#endif
