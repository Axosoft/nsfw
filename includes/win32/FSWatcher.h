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
using namespace System::Threading;
using namespace System::Windows::Threading;

ref class FSWatcher {
public:
  FSWatcher(Queue &queue, System::String ^path);
  ~FSWatcher();
  void dispatchSetup();
  void eventHandlerHelper(Action action, FileSystemEventArgs ^e);
  void onChanged(FileSystemEventArgs ^e);
  void onChangedDispatch(Object ^source, FileSystemEventArgs ^e);
  void onCreated(FileSystemEventArgs ^e);
  void onCreatedDispatch(Object ^source, FileSystemEventArgs ^e);
  void onDeleted(FileSystemEventArgs ^e);
  void onDeletedDispatch(Object ^source, FileSystemEventArgs ^e);
  void onError(ErrorEventArgs ^e);
  void onErrorDispatch(Object ^source, ErrorEventArgs ^e);
  void onRenamed(RenamedEventArgs ^e);
  void onRenamedDispatch(Object ^source, RenamedEventArgs ^e);
private:
  Dispatcher ^mDispatcher;
  ManualResetEvent ^mDispatcherStarted;
  FileSystemWatcher ^mWatcher;
  Queue &mQueue;
};

#endif
