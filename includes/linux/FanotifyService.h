#ifndef FANOTIFY_EVENT_HANDLER_H
#define FANOTIFY_EVENT_HANDLER_H

#include "FanotifyEventLoop.h"
#include "FanotifyTree.h"
#include "../Queue.h"
#include <queue>

class FanotifyEventLoop;
class FanotifyTree;

class FanotifyService {
public:
  FanotifyService(std::shared_ptr<EventQueue> queue, std::string path, const std::vector<std::string> &excludedPaths);

  std::string getError();
  bool hasErrored();
  bool isWatching();
  void updateExcludedPaths(const std::vector<std::string> &excludedPaths);

  ~FanotifyService();
private:
  void create(int wd, std::string name);
  void createDirectory(int wd, std::string name);
  void createDirectoryTree(std::string directoryTreePath);
  void dispatch(EventType action, int wd, std::string name);
  void dispatchRename(int fromWd, std::string fromName, int toWd, std::string toName);
  void modify(int wd, std::string name);
  void remove(int wd, std::string name);
  void rename(int fromWd, std::string fromName, int toWd, std::string toName);

  FanotifyEventLoop *mEventLoop;
  std::shared_ptr<EventQueue> mQueue;
  FanotifyTree *mTree;
  int mFanotifyInstance;

  friend class FanotifyEventLoop;
};

#endif
