#ifndef FILEWATCHERLINUX_H
#define FILEWATCHERLINUX_H

#include "FileWatcher.h"
#include <sys/inotify.h>
#include <dirent.h>
#include <stdlib.h>
#include <string.h>
#include <pthread.h>
#include <unistd.h>
#include <sys/select.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <errno.h>
#include <map>
#include <set>
#include <queue>

namespace NSFW {

  struct Directory {
    std::map<std::string, Directory *> childDirectories;
    std::set<std::string> files;
    std::string name, path;
    int watchDescriptor;
  };

  class FileWatcherLinux {
  public:
    FileWatcherLinux(std::string path, std::queue<Event> &eventsQueue, bool &watchFiles, Error &error);
    ~FileWatcherLinux();
    Directory *buildDirTree(std::string path, bool queueFileEvents);
    Directory *buildWatchDirectory();
    void destroyWatchTree(Directory *tree);
    std::string getPath();
    static void *mainLoop(void *params);
    void processDirectoryEvents();
    void processFileEvents();
    void setDirTree(Directory *tree);
    void setErrorMessage(std::string message);
    bool start();
    void stop();

  private:
    void addEvent(Action action, inotify_event *inEvent);
    void addEvent(Action action, std::string directory, std::string file);
    Directory *mDirTree;
    Error &mError;
    std::queue<Event> &mEventsQueue;
    int mInotify;
    std::string mPath;
    pthread_t mThread;
    std::map<int, Directory *> mWDtoDirNode;
    bool &mWatchFiles;
  };

}

#endif
