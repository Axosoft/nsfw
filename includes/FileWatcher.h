#ifndef FILEWATCHER_H
#define FILEWATCHER_H

#include "FileWatcherInterface.h"

namespace NSFW {
  class FileWatcher {
  public:
    // Constructor
    FileWatcher(std::string path);
    ~FileWatcher();

    // Public methods
    void pushEvent(Event event);
    std::queue<Event> *pollEvents();
    bool start();
    bool stop();

  private:
    FileWatcherInterface *fwInterface;
    std::queue<Event> mEventsQueue;
    std::string mPath;
    bool mWatchFiles;
  };
}

#endif
