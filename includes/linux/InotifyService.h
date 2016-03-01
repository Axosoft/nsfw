#ifndef INOTIFY_EVENT_HANDLER_H
#define INOTIFY_EVENT_HANDLER_H

#include "InotifyEventLoop.h"
#include "InotifyTree.h"
#include "../Queue.h"
#include <queue>
#include <map>
#include <iostream>

class InotifyEventLoop;
class InotifyTree;

class InotifyService {
public:
  InotifyService(Queue &queue, std::string path);
  ~InotifyService();

private:
  void create(int wd, std::string name);
  void createDirectory(int wd, std::string name);
  void createDirectoryTree(std::string directoryTreePath);
  void dispatch(EventType action, int wd, std::string name);
  void dispatchRename(int wd, std::string oldName, std::string newName);
  void modify(int wd, std::string name);
  void remove(int wd, std::string name);
  void removeDirectory(int wd);
  void rename(int wd, std::string oldName, std::string newName);
  void renameDirectory(int wd, std::string oldName, std::string newName);

  InotifyEventLoop *mEventLoop;
  Queue &mQueue;
  InotifyTree *mTree;
  int mInotifyInstance;

  friend class InotifyEventLoop;
};

#endif
