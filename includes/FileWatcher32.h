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
      FileSystemWatcher^ parentFW,
      std::queue<Event> &eventsQueue,
      bool &watchFiles,
      bool &mStopped
    );
    FSEventHandler(
      FileSystemWatcher^ parentFW,
      std::queue<Event> &eventsQueue,
      bool &watchFiles,
      bool &mStopped,
      System::String^ fileName
    );
    FileSystemWatcher ^getParent();
    bool &getStopFlag();
    bool &getWatchFiles();
    void onChanged(Object^ source, FileSystemEventArgs^ e);
    void onCreated(Object^ source, FileSystemEventArgs^ e);
    void onDeleted(Object^ source, FileSystemEventArgs^ e);
    void onRenamed(Object^ source, RenamedEventArgs^ e);
    void eventHandlerHelper(FileSystemEventArgs^ e, System::String^ action);
    void rememberHandlers(
      FileSystemEventHandler^ changed,
      FileSystemEventHandler^ created,
      FileSystemEventHandler^ deleted,
      RenamedEventHandler^ renamed
    );
    void removeHandlers();
  private:
    std::queue<Event> &mEventsQueue;
    System::String^ mFileName;
    FileSystemWatcher^ mParentFW;
    bool &mWatchFiles;
    bool &mStopFlag;
    FileSystemEventHandler^ mChanged;
    FileSystemEventHandler^ mCreated;
    FileSystemEventHandler^ mDeleted;
    RenamedEventHandler^ mRenamed;
  };

  bool createFileWatcher(std::string path, std::queue<Event> &eventsQueue, bool &watchFiles, bool &mStopped);

}

#endif
