#ifndef FILEWATCHER32_H
#define FILEWATCHER32_H

#include "FileWatcherInterface.h"
#include <vcclr.h>
#include <atlstr.h>
#include <stdio.h>
#include <string>
#include <msclr\marshal_cppstd.h>
#using <mscorlib.dll>
#using <system.dll>


using namespace System;
using namespace System::IO;
using namespace System::Runtime::InteropServices;
using namespace System::Collections::Generic;

namespace NSFW {

  ref class FSEventHandler {
  public:
    FSEventHandler(
      FileSystemWatcher^ parentFW,
      std::queue<Event> &eventsQueue,
      bool &watchFiles
    );
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
    FileSystemWatcher^ mParentFW;
    bool &mWatchFiles;
    FileSystemEventHandler^ mChanged;
    FileSystemEventHandler^ mCreated;
    FileSystemEventHandler^ mDeleted;
    RenamedEventHandler^ mRenamed;
  };

  void createFileWatcher(std::string path, std::queue<Event> &eventsQueue, bool &watchFiles);

}

#endif
