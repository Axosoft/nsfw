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

  if (!mTree->isRootExcluded()) mQueue->enqueue(action, path, name);
  // Reported first: the tree change may report creations, see renameDirectory().
  if (action != MODIFIED) {
    mTree->entryChanged(wd, name, emitCreated());
  }
}

void InotifyService::dispatchRename(int fromWd, std::string fromName, int toWd, std::string toName) {
  std::string fromPath, toPath;
  if (!mTree->getPath(fromPath, fromWd) || !mTree->getPath(toPath, toWd)) {
    return;
  }

  if (!mTree->isRootExcluded()) mQueue->enqueue(RENAMED, fromPath, fromName, toPath, toName);
  mTree->entryChanged(fromWd, fromName, emitCreated());
  mTree->entryChanged(toWd, toName, emitCreated());
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

// Either side of a move may be a directory that was dropped from the tree (moved out, into an excluded directory, or
// excluded) whose watch the kernel still has: its events queued before the drop, or the watch isn't removed yet (see
// InotifyTree::removeDroppedWatches()). The move is then only a move in or out.
void InotifyService::rename(int fromWd, std::string fromName, int toWd, std::string toName) {
  if (!mTree->nodeExists(fromWd)) {
    create(toWd, toName);
  } else if (!mTree->nodeExists(toWd)) {
    remove(fromWd, fromName);
  } else {
    dispatchRename(fromWd, fromName, toWd, toName);
  }
}

void InotifyService::createDirectory(int wd, std::string name) {
  if (!mTree->nodeExists(wd)) {
    return;
  }

  dispatch(CREATED, wd, name);
  mTree->addDirectory(wd, name, emitCreated());
}

// What the tree uses to report the contents of the directories it adds; nothing while the watched path is excluded.
EmitCreatedEvent InotifyService::emitCreated() {
  return [this](std::string directory, std::string file) {
    if (!mTree->isRootExcluded()) mQueue->enqueue(CREATED, directory, file);
  };
}

void InotifyService::removeDirectory(int wd) {
  mTree->removeDirectory(wd, emitCreated());
}

bool InotifyService::isQuarantined(int wd, uint32_t mask) {
  return mTree->isQuarantined(wd, mask);
}

void InotifyService::entryDropped(int wd, std::string name) {
  mTree->entryDropped(wd, name);
}

void InotifyService::finishRead(int eventsRead, size_t bytesRead) {
  mTree->finishRead(eventsRead, bytesRead);
}

void InotifyService::watchIgnored(int wd) {
  mTree->watchIgnored(wd);
}

bool InotifyService::removeDroppedWatch(int wd) {
  return mTree->removeDroppedWatch(wd);
}

void InotifyService::removeDroppedWatches(int eventsRead) {
  mTree->removeDroppedWatches(eventsRead);
}

void InotifyService::retryWatches() {
  mTree->retryWatches(emitCreated());
}

void InotifyService::revalidateRejected() {
  mTree->revalidateRejected(emitCreated());
}

void InotifyService::forgetIncompleteScans() {
  mTree->forgetIncompleteScans();
}

void InotifyService::removeMovedDirectory(int wd, std::string name) {
  dispatch(DELETED, wd, name);
  mTree->removeMovedDirectory(wd, name, emitCreated());
}

// See rename() for a side that isn't in the tree.
void InotifyService::renameDirectory(int fromWd, std::string fromName, int toWd, std::string toName) {
  if (!mTree->nodeExists(fromWd)) {
    createDirectory(toWd, toName);
    return;
  }
  if (!mTree->nodeExists(toWd)) {
    removeMovedDirectory(fromWd, fromName);
    return;
  }

  // Reported first: the tree change may drop the node of the directory it left (see InotifyTree::discard()),
  // and report creations under the new path, which the rename has to come before.
  dispatchRename(fromWd, fromName, toWd, toName);
  mTree->renameDirectory(fromWd, fromName, toWd, toName, emitCreated());
}

void InotifyService::setError(const std::string &error) {
  mTree->setError(error);
}

std::future<bool> InotifyService::updateExcludedPaths(const std::vector<std::string> &excludedPaths) {
  // Adds and removes tree nodes, which the event loop thread reads and changes while it handles events.
  std::unique_lock<std::mutex> lock = mEventLoop->lockEventHandling();
  auto completion = mTree->updateExcludedPaths(excludedPaths, emitCreated());
  // The event loop may be waiting for events, and the first removals wake it up for the rest, and for the retries.
  mTree->removeDroppedWatches(0);
  mEventLoop->wakeForEntryRetry();
  return completion;
}