#ifndef FILEWATCHER32_H
#define FILEWATCHER32_H

#include "FileWatcher.h"
#include <string>
#include <msclr\marshal_cppstd.h>
#using <mscorlib.dll>
#using <system.dll>


using namespace System;
using namespace System::IO;
using namespace System::Runtime::InteropServices;
using namespace System::Collections::Generic;
using namespace System::Threading;

namespace NSFW {

  ref class FSEventHandler {
  public:
    FSEventHandler(
      FileSystemWatcher ^parentFW,
      EventQueue &eventQueue,
      bool &watchFiles,
      bool &stopped,
      Error &error
    );
    FSEventHandler(
      FileSystemWatcher ^parentFW,
      EventQueue &eventQueue,
      bool &watchFiles,
      bool &stopped,
      Error &error,
      System::String ^fileName
    );
    void eventHandlerHelper(FileSystemEventArgs ^e, Action action);
    FileSystemWatcher ^getParent();
    Error &getErrorStruct();
    bool &getStopFlag();
    bool &getWatchFiles();
    void onChanged(Object ^source, FileSystemEventArgs ^e);
    void onCreated(Object ^source, FileSystemEventArgs ^e);
    void onDeleted(Object ^source, FileSystemEventArgs ^e);
    void onError(Object ^source, ErrorEventArgs ^e);
    void onRenamed(Object ^source, RenamedEventArgs ^e);
    void rememberHandlers(
      FileSystemEventHandler ^changedHandler,
      FileSystemEventHandler ^createdHandler,
      FileSystemEventHandler ^deletedHandler,
      ErrorEventHandler ^errorHandler,
      RenamedEventHandler ^renamedHandler
    );
    void removeHandlers();
  private:
    FileSystemEventHandler ^mChangedHandler;
    FileSystemEventHandler ^mCreatedHandler;
    FileSystemEventHandler ^mDeletedHandler;
    Error &mError;
    ErrorEventHandler ^mErrorHandler;
    EventQueue &mEventQueue;
    System::String ^mFileName;
    FileSystemWatcher ^mParentFW;
    RenamedEventHandler ^mRenamedHandler;
    bool &mStopFlag;
    bool &mWatchFiles;
  };

  bool createFileWatcher(std::string path, EventQueue &eventQueue, bool &watchFiles, bool &stopped, Error &error);
  System::String^ getDirectoryName(System::String^ path);
  System::String^ getFileName(System::String^ path);
}

#endif
