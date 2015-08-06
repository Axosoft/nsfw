#ifndef FILEWATCHERINTERFACE_H
#define FILEWATCHERINTERFACE_H

#include "FileWatcher.h";

namespace NSFW {
  class FileWatcherInterface {
  public:
    FileWatcherInterface(std::string path, std::queue<Event> &eventsQueue);
    virtual ~FileWatcherInterface();
  };
}

#endif
