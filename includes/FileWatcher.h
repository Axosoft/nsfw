#ifndef FILEWATCHER_H
#define FILEWATCHER_H

#include "EventQueue.h"
#include <string>

namespace NSFW {

  struct Error {
    std::string message;
    bool status;
  };

  class FileWatcher {
  public:
    // Constructor
    FileWatcher(std::string path);
    ~FileWatcher();

    // Public methods
    std::string errorMessage();
    bool errors();
    bool hasStopped();
    EventQueue &getEventQueue();
    bool running();
    bool start();
    bool stop();

  private:
    void *fwInterface;
    Error mError;
    EventQueue mEventQueue;
    std::string mPath;
    bool mStopFlag;
    bool mWatchFiles;
  };
}

#endif
