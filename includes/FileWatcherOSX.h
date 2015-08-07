#ifndef FILEWATCHEROSX_H
#define FILEWATCHEROSX_H

#include "FileWatcherInterface.h"

namespace NSFW {

  class FileWatcherOSX : public FileWatcherInterface {
  public:
    FileWatcherOSX(std::string path, std::queue<Event> &eventsQueue, bool &watchFiles);
  private:
    std::queue<Event> &mEventsQueue;
    std::string mPath;
    bool &mWatchFiles;
  };

}

#endif
