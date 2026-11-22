#ifndef INOTIFY_EVENT_HANDLER_H
#define INOTIFY_EVENT_HANDLER_H

#include "InotifyEventLoop.h"
#include "InotifyTree.h"
#include "../Queue.h"
#include <queue>
#include <map>

class InotifyEventLoop;
class InotifyTree;

class InotifyService {
public:
  InotifyService(std::shared_ptr<EventQueue> queue, std::string path, const std::vector<std::string> &excludedPaths);

  std::string getError();
  bool hasErrored();
  bool isWatching();
  std::future<bool> updateExcludedPaths(const std::vector<std::string> &excludedPaths);

  ~InotifyService();
private:
  void create(int wd, std::string name, bool fromInCreate);
  void createDirectory(int wd, std::string name, bool fromInCreate);
  void createDirectoryTree(std::string directoryTreePath);
  void dispatch(EventType action, int wd, std::string name, bool fromInCreate = false);
  void dispatchRename(int fromWd, std::string fromName, int toWd, std::string toName);
  EmitCreatedEvent emitCreated();
  void watchIgnored(int wd);
  bool isQuarantined(int wd, uint32_t mask);
  void entryDropped(int wd, std::string name);
  void startRead(size_t bytesRead);
  void finishRead(int eventsRead, size_t bytesRead);
  void modify(int wd, std::string name);
  void remove(int wd, std::string name);
  void removeDirectory(int wd);
  void forgetIncompleteScans();
  bool removeDroppedWatch(int wd);
  void removeDroppedWatches(int eventsRead);
  void retryWatches();
  void revalidateRejected();
  void revalidateLinks(bool unknownMoveDestination = false);
  void removeMovedDirectory(int wd, std::string name);
  void rename(int fromWd, std::string fromName, int toWd, std::string toName);
  void renameDirectory(int fromWd, std::string fromName, int toWd, std::string toName);
  void setError(const std::string &error);

  InotifyEventLoop *mEventLoop;
  std::shared_ptr<EventQueue> mQueue;
  InotifyTree *mTree;
  int mInotifyInstance;

  friend class InotifyEventLoop;
};

#endif
