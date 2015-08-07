#ifndef FILEWATCHEROSX_H
#define FILEWATCHEROSX_H

#include "FileWatcherInterface.h"
#include <CoreServices/CoreServices.h>
#include <pthread.h>

namespace NSFW {

  class FileWatcherOSX : public FileWatcherInterface {
  public:
    FileWatcherOSX(std::string path, std::queue<Event> &eventsQueue, bool &watchFiles);
    static void callback(
      ConstFSEventStreamRef streamRef,
      void *clientCallBackInfo,
      size_t numEvents,
      void *eventPaths,
      const FSEventStreamEventFlags eventFlags[],
      const FSEventStreamEventId eventIds[]
    );
    std::string getPath();
    static void *mainLoop(void *params);
    bool start();
  private:
    std::queue<Event> &mEventsQueue;
    std::string mPath;
    pthread_t mThread;
    bool &mWatchFiles;
  };

}

#endif
