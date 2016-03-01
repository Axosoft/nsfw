#include "../../includes/linux/InotifyService.h"

InotifyService::InotifyService(Queue &queue, std::string path):
  mQueue(queue) {
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
