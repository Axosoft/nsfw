#ifndef NOTIFY_EVENT_HANDLER_H
#define NOTIFY_EVENT_HANDLER_H

#include "InotifyService.h"
#include "FanotifyService.h"
#include "../Queue.h"
#include <queue>
#include <map>

class InotifyService;
class FanotifyService;

class NotifyService {
public:
  NotifyService(std::shared_ptr<EventQueue> queue, std::string path, const std::vector<std::string> &excludedPaths);

  std::string getError();
  bool hasErrored();
  bool isWatching();
  void updateExcludedPaths(const std::vector<std::string> &excludedPaths);

  ~NotifyService();
private:
  InotifyService *mInotifyService;
  FanotifyService *mFanotifyService;

  friend class InotifyService;
  friend class FanotifyService;
};

#endif
