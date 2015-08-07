#ifndef FILEWATCHER_H
#define FILEWATCHER_H

#include "FileWatcherInterface.h"

#if defined(_WIN32)
#include "FileWatcher32.h"
#define USE_WINDOWS_INIT
#elif defined(__APPLE_CC__) || defined(BSD)
#include "FileWatcherOSX.h"
#define FILE_WATCHER_INTERFACE FileWatcherOSX
#elif defined(__linux__)
#define FILE_WATCHER_INTERFACE FileWatcherLinux
#endif

namespace NSFW {
  class FileWatcher {
  public:
    // Constructor
    FileWatcher(std::string path);
    ~FileWatcher();

    // Public methods
    void pushEvent(Event event);
    std::queue<Event> *pollEvents();
    bool running();
    bool start();
    bool stop();

  private:
    FileWatcherInterface *fwInterface;
    std::queue<Event> mEventsQueue;
    std::string mPath;
    bool mWatchFiles;
  };
}

#endif
