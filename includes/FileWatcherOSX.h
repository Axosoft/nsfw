#ifndef FILEWATCHEROSX_H
#define FILEWATCHEROSX_H

#include "FileWatcher.h"
#include <CoreServices/CoreServices.h>
#include <pthread.h>
#include <unistd.h>
#include <stdlib.h>
#include <stdio.h>
#include <dirent.h>
#include <sys/stat.h>
#include <string.h>
#include <search.h>
#include <time.h>
#include <sys/errno.h>
#include <map>
#include <queue>

namespace NSFW
{
  struct FileDescriptor
  {
    struct stat meta;
    std::string name, path;
  };

  struct FilePoll
  {
    struct stat file;
    bool exists;
  };

  struct Directory
  {
    std::map<ino_t, FileDescriptor> fileMap;
    std::map<ino_t, Directory *> childDirectories;
    std::string name, path;
  };

  struct DirectoryPair
  {
    Directory *prev, *current;
  };

  class FileWatcherOSX
  {
  public:
    FileWatcherOSX(std::string path, EventQueue &eventQueue, bool &watchFiles, Error &error);
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
    void handleTraversingDirectoryChange(Action action, Directory *directory);
    bool isSingleFileWatch();
    static void *mainLoop(void *params);
    bool mDie;
    Directory *mDirTree;
    FilePoll mFile;
    void processDirCallback();
    void filePoller();
    void setErrorMessage(std::string message);
    Directory *snapshotDir();
    bool start();
    void stop();
    static void timerCallback(CFRunLoopTimerRef timer, void* callbackInfo);
    FSEventStreamRef mStream;
    CFRunLoopRef mRunLoop;
  private:
    void deleteDirTree(Directory *tree);
    pthread_mutex_t mCallbackSync;
    Error &mError;
    EventQueue &mEventQueue;
    bool mIsDirWatch;
    pthread_mutex_t mMainLoopSync;
    std::string mPath;
    pthread_t mThread;
    bool &mWatchFiles;
  };
}

#endif
