#ifndef FILEWATCHERLINUX_H
#define FILEWATCHERLINUX_H

#include <sys/inotify.h>
#include <dirent.h>
#include <stdlib.h>
#include <string.h>
#include <pthread.h>
#include <unistd.h>
#include <map>
#include <set>
#include <sys/select.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <errno.h>

namespace NSFW {

  struct Directory {
    std::map<std::string, Directory *> childDirectories;
    std::set<std::string> files;
    std::string name, path;
    int watchDescriptor;
  };

  class FileWatcherLinux {
  public:
    FileWatcherLinux(std::string path, std::queue<Event> &eventsQueue, bool &watchFiles);
    ~FileWatcherLinux();
    void addEvent(std::string action, inotify_event *inEvent);
    void addEvent(std::string action, std::string directory, std::string *file);
    Directory *buildDirTree(std::string path, bool queueFileEvents);
    void destroyWatchTree(Directory *tree);
    std::string getPath();
    static void *mainLoop(void *params);
    void processEvents();
    void setDirTree(Directory *tree);
    bool start();
    void stop();

  private:
    Directory *mDirTree;
    std::queue<Event> &mEventsQueue;
    int mInotify;
    std::string mPath;
    pthread_t mThread;
    std::map<int, Directory *> mWDtoDirNode;
    bool &mWatchFiles;
  };

}

#endif
