#include "../../includes/linux/InotifyService.h"

InotifyService::InotifyService(EventQueue &queue, std::string path):
  mEventLoop(NULL),
  mQueue(queue),
  mTree(NULL) {
  mInotifyInstance = inotify_init();

  if (mInotifyInstance == -1) {
    return;
  }

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

  close(mInotifyInstance);
}

void InotifyService::create(int wd, std::string name) {
  dispatch(CREATED, wd, name);
}

void InotifyService::dispatch(EventType action, int wd, std::string name) {
  std::string path;
  if (!mTree->getPath(path, wd)) {
    return;
  }

  mQueue.enqueue(action, path, name);
}

void InotifyService::dispatchRename(int wd, std::string oldName, std::string newName) {
  std::string path;
  if (!mTree->getPath(path, wd)) {
    return;
  }

  mQueue.enqueue(RENAMED, path, oldName, newName);
}

std::string InotifyService::getError() {
  if (!isWatching()) {
    return "Service shutdown unexpectedly";
  }

  if (mTree->hasErrored()) {
    return mTree->getError();
  }

  return "";
}

bool InotifyService::hasErrored() {
  return !isWatching() || (mTree == NULL ? false : mTree->hasErrored());
}

bool InotifyService::isWatching() {
  if (mTree == NULL || mEventLoop == NULL) {
    return false;
  }

  return mTree->isRootAlive() && mEventLoop->isLooping();
}

void InotifyService::modify(int wd, std::string name) {
  dispatch(MODIFIED, wd, name);
}

void InotifyService::remove(int wd, std::string name) {
  dispatch(DELETED, wd, name);
}

void InotifyService::rename(int wd, std::string oldName, std::string newName) {
  dispatchRename(wd, oldName, newName);
}

void InotifyService::createDirectory(int wd, std::string name) {
  if (!mTree->nodeExists(wd)) {
    return;
  }

  mTree->addDirectory(wd, name);
  dispatch(CREATED, wd, name);
}

void InotifyService::removeDirectory(int wd) {
  mTree->removeDirectory(wd);
}

void InotifyService::renameDirectory(int wd, std::string oldName, std::string newName) {
  if (!mTree->nodeExists(wd)) {
    return;
  }

  mTree->renameDirectory(wd, oldName, newName);

  dispatchRename(wd, oldName, newName);
}
