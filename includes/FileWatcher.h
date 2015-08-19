#ifndef FILEWATCHER_H
#define FILEWATCHER_H

#include <string>
#include <queue>

namespace NSFW {

  struct Event {
    std::string action;
    std::string directory;
    std::string *file;
  };

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
    std::queue<Event> *pollEvents();
    bool running();
    bool start();
    bool stop();

  private:
    void *fwInterface;
    Error mError;
    std::queue<Event> mEventsQueue;
    std::string mPath;
    bool mStopFlag;
    bool mWatchFiles;
  };
}

#endif
