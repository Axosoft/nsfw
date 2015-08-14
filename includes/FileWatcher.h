#ifndef FILEWATCHER_H
#define FILEWATCHER_H

#include "FileWatcherInterface.h"

#if defined(_WIN32)
#define USE_WINDOWS_INIT
#include "FileWatcher32.h"
#elif defined(__APPLE_CC__) || defined(BSD)
#define FILE_WATCHER_INTERFACE FileWatcherOSX
#include "FileWatcherOSX.h"
#elif defined(__linux__)
#define FILE_WATCHER_INTERFACE FileWatcherLinux
#include "FileWatcherLinux.h"
#endif


namespace NSFW {
  class FileWatcher {
  public:
    // Constructor
    FileWatcher(std::string path);
    ~FileWatcher();

    // Public methods
    bool hasStopped();
    void pushEvent(Event event);
    std::queue<Event> *pollEvents();
    bool running();
    bool start();
    bool stop();

  private:
    FILE_WATCHER_INTERFACE *fwInterface;
    std::queue<Event> mEventsQueue;
    std::string mPath;
    bool mStopFlag;
    bool mWatchFiles;
  };
}

#endif
