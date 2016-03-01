#ifndef INOTIFY_EVENT_HANDLER_H
#define INOTIFY_EVENT_HANDLER_H

#include "InotifyEventLoop.h"
#include "InotifyTree.h"
#include <queue>
#include <map>
#include <iostream>

class InotifyEventLoop;
class InotifyTree;

class InotifyService {
public:
  InotifyService(std::string path);
  ~InotifyService();

private:
  void create(int wd, std::string name);
  void createDirectory(int wd, std::string name);
  void createDirectoryTree(std::string directoryTreePath);
  void modify(int wd, std::string name);
  void remove(int wd, std::string name);
  void removeDirectory(int wd);
  void rename(int wd, std::string oldName, std::string newName);
  void renameDirectory(int wd, std::string oldName, std::string newName);

  InotifyEventLoop *mEventLoop;
  InotifyTree *mTree;
  int mInotifyInstance;
  int mAttributes;

  friend class InotifyEventLoop;
};

#endif
