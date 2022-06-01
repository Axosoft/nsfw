#ifndef NSFW_WIN32_CONTROLLER
#define NSFW_WIN32_CONTROLLER

#include <string>
#include <memory>
#include "Watcher.h"

class EventQueue;

class Controller {
  public:
    Controller(std::shared_ptr<EventQueue> queue, const std::string &path, const std::vector<std::string> &excludedPaths);

    std::string getError();
    bool hasErrored();
    bool isWatching();
    void updateExcludedPaths(const std::vector<std::string> &excludedPaths);

    ~Controller();
  private:
    std::unique_ptr<Watcher> mWatcher;
};

#endif
