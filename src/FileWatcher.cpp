#include "../includes/FileWatcher.h"
#include <iostream>
#if defined(_WIN32)
#define USE_WINDOWS_INIT
#include "../includes/FileWatcher32.h"
#elif defined(__APPLE_CC__) || defined(BSD)
#define FILE_WATCHER_INTERFACE FileWatcherOSX
#include "../includes/FileWatcherOSX.h"
#elif defined(__linux__)
#define FILE_WATCHER_INTERFACE FileWatcherLinux
#include "../includes/FileWatcherLinux.h"
#endif

namespace NSFW {

  #pragma unmanaged

  FileWatcher::FileWatcher(std::string path)
   : mPath(path), mStopFlag(false), mWatchFiles(false) {}

  FileWatcher::~FileWatcher() {
  }

  std::string FileWatcher::errorMessage() {
    return mError.message;
  }

  bool FileWatcher::errors() {
    return mError.status;
  }

  bool FileWatcher::hasStopped() {
    return mStopFlag;
  }

  EventQueue &FileWatcher::getEventQueue() {
    return mEventQueue;
  }

  bool FileWatcher::running() {
    return mWatchFiles;
  }

  bool FileWatcher::start() {
    if (!mWatchFiles) {
      mWatchFiles = true;
      mError.status = false;
      // normalization
      if (mPath[mPath.length() - 1] == '/' || mPath[mPath.length() - 1] == '\\') {
        mPath = mPath.substr(0, mPath.length() - 2);
      }

      mEventQueue.clear();

      #if defined(USE_WINDOWS_INIT)
      createFileWatcher(mPath, mEventQueue, mWatchFiles, mStopFlag, mError);
      #else
      fwInterface = (void *) new FILE_WATCHER_INTERFACE(mPath, mEventQueue, mWatchFiles, mError);
      ((FILE_WATCHER_INTERFACE *)fwInterface)->start();
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
      ((FILE_WATCHER_INTERFACE *)fwInterface)->stop();
      delete (FILE_WATCHER_INTERFACE *)fwInterface;
      fwInterface = NULL;
      mStopFlag = true;
      #endif
      mEventQueue.clear();
      return true;
    }
  }

}
