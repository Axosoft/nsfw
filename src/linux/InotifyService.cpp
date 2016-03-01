#include "../../includes/linux/InotifyService.h"

void writeLog(std::string type, std::string path) {
  std::cout
    << type
    << ": "
    << path
    << std::endl;
}

InotifyService::InotifyService(std::string path) {
  // TODO: add failure catches
  mInotifyInstance = inotify_init();

  mTree = new InotifyTree(mInotifyInstance, path);
  if (!mTree->isRootAlive()) {
    delete mTree;
    mTree = NULL;
    mEventLoop = NULL;
  } else {
    mEventLoop = new InotifyEventLoop(
      mInotifyInstance,
      this
    );
  }
}

InotifyService::~InotifyService() {
  if (mEventLoop != NULL) {
    delete mEventLoop;
  }

  if (mTree != NULL) {
    delete mTree;
  }
}

void InotifyService::create(int wd, std::string name) {
  std::string fullName;
  if (!mTree->getPath(fullName, wd)) {
    return;
  }

  fullName += std::string("/") + name;
  writeLog("CREATE", fullName);
}

void InotifyService::modify(int wd, std::string name) {
  std::string fullName;
  if (!mTree->getPath(fullName, wd)) {
    return;
  }

  fullName += std::string("/") + name;
  writeLog("MODIFY", fullName);
}

void InotifyService::remove(int wd, std::string name) {
  std::string fullName;
  if (!mTree->getPath(fullName, wd)) {
    return;
  }

  fullName += std::string("/") + name;
  writeLog("REMOVE", fullName);
}

void InotifyService::rename(int wd, std::string oldName, std::string newName) {
  std::string path;
  if (!mTree->getPath(path, wd)) {
    return;
  }

  writeLog("RENAME A", path + "/" + oldName);
  writeLog("RENAME B", path + "/" + newName);
}

void InotifyService::createDirectory(int wd, std::string name) {
  std::string fullName;
  if (!mTree->getPath(fullName, wd)) {
    return;
  }

  fullName += std::string("/") + name;
  mTree->addDirectory(wd, name);
  writeLog("CREATE", fullName);
}

void InotifyService::removeDirectory(int wd) {
  mTree->removeDirectory(wd);
}

void InotifyService::renameDirectory(int wd, std::string oldName, std::string newName) {
  std::string path;
  if (!mTree->getPath(path, wd)) {
    return;
  }

  mTree->renameDirectory(wd, oldName, newName);

  writeLog("RENAME A", path + "/" + oldName);
  writeLog("RENAME B", path + "/" + newName);
}
