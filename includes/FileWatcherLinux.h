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

#define NO_WATCH -1
#define DEAD_NODE -2

namespace NSFW
{
  struct Directory
  {
    std::map<std::string, Directory *> childDirectories;
    std::set<std::string> files;
    bool isNew;
    std::string name, path;
    Directory *parent;
    int watchDescriptor; // can be flagged as NO_WATCH or DEAD_NODE
  };

  class FileWatcherLinux
  {
  public:
    FileWatcherLinux(std::string path, EventQueue &eventQueue, bool &watchFiles, Error &error);
    ~FileWatcherLinux();

    bool buildWatchDirectory();
    bool buildWatchTree();
    void destroyWatchTree(Directory *tree, std::set<Directory *> *cleanUp = NULL);
    Directory *findFirstDeadAncestor(Directory *child);
    std::string getPath();
    static void *mainLoop(void *params);
    void processDirectoryEvents();
    void processFileEvents();
    bool refreshWatchTree();
    void setErrorMessage(std::string message);
    bool start();
    void stop();

  private:
    bool addDirectoryEvent(inotify_event *inEvent);
    void addEvent(Action action, inotify_event *inEvent);
    void addEvent(Action action, std::string directory, std::string fileA, std::string fileB = "");
    Directory *mDirTree;
    Error &mError;
    EventQueue &mEventQueue;
    int mInotify;
    pthread_mutex_t mMainLoopSync;
    std::string mPath;
    pthread_t mThread;
    std::map<int, Directory *> mWDtoDirNode;
    bool &mWatchFiles;
  };
}

#endif
