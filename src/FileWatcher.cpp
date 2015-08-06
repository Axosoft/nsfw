#include "FileWatcher.h"
#include "FileWatcherInterface.h"

#if defined(_WIN32)
#include "includes/FileWatcher32.h"
#define FILE_WATCHER_INTERFACE FileWatcher32
#elif defined(__APPLE_CC__) || defined(BSD)
#elif defined(__linux__)
#endif

namespace NSFW {

  FileWatcher::FileWatcher() {
    fwInterface = new FILE_WATCHER_INTERFACE(mEventsQueue);
  }
  FileWatcher::~FileWatcher() {
    delete fwInterface;
  }

  // Internal methods
  void FileWatcher::pushEvent(Event event) {}
  std::queue<Event> *FileWatcher::pollEvents() {
    return 0;
  }
}
