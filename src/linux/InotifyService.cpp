#include "../../includes/linux/InotifyService.h"

#ifdef NSFW_TEST_SLOW_1
// Lets js/spec/index-slow-spec.js stop the event loop before start() checks that the watcher runs: when the watched
// directory holds this file, the loop's first read() fails.
static bool failFirstRead(const std::string &path, int inotifyInstance) {
  struct stat file;
  if (stat((path + "/nsfw-test-fail-first-read").c_str(), &file) != 0) {
    return false;
  }
  return InotifyEventLoop::failReads(inotifyInstance);
}
#endif

InotifyService::InotifyService(std::shared_ptr<EventQueue> queue, std::string path, const std::vector<std::string> &excludedPaths):
  mEventLoop(NULL),
  mQueue(queue),
  mTree(NULL) {
  mInotifyInstance = inotify_init();

  if (mInotifyInstance == -1) {
    return;
  }

  mTree = new InotifyTree(mInotifyInstance, path, excludedPaths);
  if (!mTree->isRootAlive()) {
    delete mTree;
    mTree = NULL;
    mEventLoop = NULL;
    close(mInotifyInstance);
  } else {
    #ifdef NSFW_TEST_SLOW_1
      bool failingFirstRead = failFirstRead(path, mInotifyInstance);
    #endif
    mEventLoop = new InotifyEventLoop(
      mInotifyInstance,
      this
    );
    #ifdef NSFW_TEST_SLOW_1
      // Waits for the loop to record the error, plus a margin for it to finish, so it stops before start()'s check.
      for (int i = 0; failingFirstRead && i < 1000 && !mTree->hasErrored(); ++i) {
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
      }
      if (failingFirstRead) {
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
      }
    #endif
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

  mQueue->enqueue(action, path, name);
}

void InotifyService::dispatchRename(int fromWd, std::string fromName, int toWd, std::string toName) {
  std::string fromPath, toPath;
  if (!mTree->getPath(fromPath, fromWd) || !mTree->getPath(toPath, toWd)) {
    return;
  }

  mQueue->enqueue(RENAMED, fromPath, fromName, toPath, toName);
}

std::string InotifyService::getError() {
  if (mTree != NULL && mTree->hasErrored()) {
    return mTree->getError();
  }

  if (!isWatching()) {
    return "Service shutdown unexpectedly";
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

  return mTree->isRootAlive() && mEventLoop->isStarted();
}

void InotifyService::modify(int wd, std::string name) {
  dispatch(MODIFIED, wd, name);
}

void InotifyService::remove(int wd, std::string name) {
  dispatch(DELETED, wd, name);
}

void InotifyService::rename(int fromWd, std::string fromName, int toWd, std::string toName) {
  dispatchRename(fromWd, fromName, toWd, toName);
}

void InotifyService::createDirectory(int wd, std::string name) {
  if (!mTree->nodeExists(wd)) {
    return;
  }

  dispatch(CREATED, wd, name);
  mTree->addDirectory(wd, name, [this](std::string directory, std::string file) {
    mQueue->enqueue(CREATED, directory, file);
  });
}

void InotifyService::removeDirectory(int wd) {
  mTree->removeDirectory(wd);
}

void InotifyService::renameDirectory(int fromWd, std::string fromName, int toWd, std::string toName) {
  if (!mTree->nodeExists(fromWd) || !mTree->nodeExists(toWd)) {
    return;
  }

  mTree->renameDirectory(fromWd, fromName, toWd, toName);

  dispatchRename(fromWd, fromName, toWd, toName);
}

void InotifyService::setError(const std::string &error) {
  mTree->setError(error);
}

void InotifyService::updateExcludedPaths(const std::vector<std::string> &excludedPaths) {
  // Adds and removes tree nodes, which the event loop thread reads and changes while it handles events.
  std::unique_lock<std::mutex> lock = mEventLoop->lockEventHandling();
  mTree->updateExcludedPaths(excludedPaths);
}