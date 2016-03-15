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
using namespace System::Security::Permissions;

ref class FSWatcher {
public:
  FSWatcher(EventQueue &queue, System::String ^path);
  ~FSWatcher();
  void dispatchSetup();
  void eventHandlerHelper(EventType action, FileSystemEventArgs ^e);
  System::String ^getError();
  bool hasErrored();
  void onChangedDispatch(Object ^source, FileSystemEventArgs ^e);
  void onCreatedDispatch(Object ^source, FileSystemEventArgs ^e);
  void onDeletedDispatch(Object ^source, FileSystemEventArgs ^e);
  void onErrorDispatch(Object ^source, ErrorEventArgs ^e);
  void onRenamed(RenamedEventArgs ^e);
  void onRenamedDispatch(Object ^source, RenamedEventArgs ^e);
private:
  ConcurrentQueue< Tuple<Int32, Object ^> ^> ^mDispatchQueue;
  bool mDispatching;
  Thread ^mDispatchThread;
  System::String ^mError;
  FileSystemWatcher ^mWatcher;
  EventQueue &mQueue;
};

#endif
