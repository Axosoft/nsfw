#ifndef FILEWATCHEROSX_H
#define FILEWATCHEROSX_H

#include "FileWatcherInterface.h"
#include <CoreServices/CoreServices.h>
#include <pthread.h>
#include <unistd.h>
#include <stdlib.h>
#include <stdio.h>
#include <dirent.h>
#include <sys/stat.h>
#include <string.h>
#include <search.h>

namespace NSFW {

  struct Directory {
    dirent *entry;
    dirent **entries;
    size_t numEntries;
    Directory *childDirectories;
    size_t numChildren;
    std::string path;
  };

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
    void snapshotDir();
    bool start();
  private:
    std::queue<Event> &mEventsQueue;
    std::string mPath;
    pthread_t mThread;
    bool &mWatchFiles;
  };

}

#endif
