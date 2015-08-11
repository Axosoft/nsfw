#ifndef FILEWATCHERLINUX_H
#define FILEWATCHERLINUX_H

#include "FileWatcherInterface.h"

namespace NSFW {

  class FileWatcherLinux : public FileWatcherInterface {
  public:
    FileWatcherLinux(std::string path, std::queue<Event> &eventsQueue, bool &watchFiles);
    ~FileWatcherLinux();
    bool start();
    void stop();

  private:
    std::queue<Event> &mEventsQueue;
    std::string mPath;
    bool &mWatchFiles;
  };

}

#endif
