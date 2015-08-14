#ifndef FILEWATCHEROSX_H
#define FILEWATCHEROSX_H

#include <CoreServices/CoreServices.h>
#include <pthread.h>
#include <unistd.h>
#include <stdlib.h>
#include <stdio.h>
#include <dirent.h>
#include <sys/stat.h>
#include <string.h>
#include <search.h>
#include <map>
#include <time.h>

namespace NSFW {

  struct FileDescriptor {
    dirent *entry;
    struct stat meta;
    std::string path;
  };

  struct Directory {
    dirent *entry;
    std::map<ino_t, FileDescriptor> fileMap;
    std::map<ino_t, Directory *> childDirectories;
    std::string path;
  };

  struct DirectoryPair {
    Directory *prev, *current;
  };

  class FileWatcherOSX {
  public:
    FileWatcherOSX(std::string path, std::queue<Event> &eventsQueue, bool &watchFiles);
    ~FileWatcherOSX();
    static void callback(
      ConstFSEventStreamRef streamRef,
      void *clientCallBackInfo,
      size_t numEvents,
      void *eventPaths,
      const FSEventStreamEventFlags eventFlags[],
      const FSEventStreamEventId eventIds[]
    );
    static bool checkTimeValEquality(struct timespec *x, struct timespec *y);
    std::string getPath();
    void handleTraversingDirectoryChange(std::string action, Directory *directory);
    static void *mainLoop(void *params);
    void processCallback();
    Directory *snapshotDir();
    bool start();
    void stop();

    Directory *mDirTree;

  private:
    void deleteDirTree(Directory *tree);
    pthread_mutex_t mCallbackSynch;
    std::queue<Event> &mEventsQueue;
    std::string mPath;
    pthread_t mThread;
    bool &mWatchFiles;
  };

}

#endif
