#ifndef FILEWATCHER32_H
#define FILEWATCHER32_H
#using <system.dll>
using namespace System;
using namespace System::IO;

#include "FileWatcherInterface.h"

namespace NSFW {

  ref class FSEventHandler {
  public:
    FSEventHandler(std::string path, std::queue<Event> &eventsQueue);
    void OnChanged(Object^ source, FileSystemEventArgs^ e);
    void OnRenamed(Object^ source, RenamedEventArgs^ e);
  private:
    std::queue<Event> &mEventsQueue;
  };

  class FileWatcher32 : public FileWatcherInterface {
  public:
    FileWatcher32(std::queue<Event> &eventsQueue);
    ~FileWatcher32();
  private:
    FileSystemWatcher^ fsWatcher;
    FSEventHandler^ handler;
  };

}

#endif
