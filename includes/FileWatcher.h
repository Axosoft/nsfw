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
    void *fwInterface;
    std::queue<Event> mEventsQueue;
    std::string mPath;
    bool mStopFlag;
    bool mWatchFiles;
  };
}

#endif
