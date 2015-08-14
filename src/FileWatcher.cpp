#include "../includes/FileWatcher.h"

#if defined(_WIN32)
#include "../includes/FileWatcher32.h"
#elif defined(__APPLE_CC__) || defined(BSD)
#include "../includes/FileWatcherOSX.h"
#elif defined(__linux__)
#include "../includes/FileWatcherLinux.h"
#endif

namespace NSFW {

  FileWatcher::FileWatcher(std::string path)
   : mPath(path), mStopFlag(false), mWatchFiles(false) {}

  FileWatcher::~FileWatcher() {
    #ifndef USE_WINDOWS_INIT
    delete fwInterface;
    #endif
  }

  // Public methods
  bool FileWatcher::running() {
    return mWatchFiles;
  }

  bool FileWatcher::hasStopped() {
    return mStopFlag;
  }

  bool FileWatcher::start() {
    if (!mWatchFiles) {
      mWatchFiles = true;

      // normalization
      if (mPath[mPath.length() - 1] == '/' || mPath[mPath.length() - 1] == '\\') {
        mPath = mPath.substr(0, mPath.length() - 2);
      }

      #if defined(USE_WINDOWS_INIT)
      createFileWatcher(mPath, mEventsQueue, mWatchFiles, mStopFlag);
      #else
      fwInterface = new FILE_WATCHER_INTERFACE(mPath, mEventsQueue, mWatchFiles);
      fwInterface->start();
      #endif
      return true;
    } else {
      return false;
    }
  }

  bool FileWatcher::stop() {
    if (!mWatchFiles) {
      return false;
    } else {
      mWatchFiles = false;
      #ifndef USE_WINDOWS_INIT
      fwInterface->stop();
      delete fwInterface;
      mStopFlag = true;
      #endif
      return true;
    }
  }

  // Internal methods
  std::queue<Event> *FileWatcher::pollEvents() {
    if (mEventsQueue.empty()) {
      return NULL;
    }
    std::queue<Event> *out = new std::queue<Event>();
    std::swap(mEventsQueue, *out);
    return out;
  }

}
