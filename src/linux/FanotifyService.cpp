#include "../../includes/linux/FanotifyService.h"

FanotifyService::FanotifyService(std::shared_ptr<EventQueue> queue, std::string path, const std::vector<std::string> &excludedPaths):
  mEventLoop(NULL),
  mQueue(queue),
  mTree(NULL) {
  
  #ifdef FAN_REPORT_FID
    mFanotifyInstance = fanotify_init(FAN_CLASS_NOTIF | FAN_REPORT_FID | FAN_REPORT_DIR_FID | FAN_REPORT_NAME, O_LARGEFILE);
    
    if (mFanotifyInstance >= 0) {
      uint64_t mask = FAN_MODIFY | FAN_ATTRIB | FAN_CREATE | FAN_DELETE | FAN_MOVE | FAN_ONDIR;
      int res = fanotify_mark(mFanotifyInstance, FAN_MARK_ADD | FAN_MARK_FILESYSTEM, mask, AT_FDCWD, path.c_str());

      if (res >= 0) {
        mEventLoop = new FanotifyEventLoop(
          mFanotifyInstance,
          this
        );
        mTree = new FanotifyTree(mFanotifyInstance, path, excludedPaths);
        return;
      }
    }
  #endif
}

FanotifyService::~FanotifyService() {
  if (mEventLoop != NULL) {
    delete mEventLoop;
  }

  if (mTree != NULL) {
    delete mTree;
  }

  close(mFanotifyInstance);
}

void FanotifyService::create(int wd, std::string name) {
  dispatch(CREATED, wd, name);
}

void FanotifyService::dispatch(EventType action, int wd, std::string name) {
  std::string path;

  if (mTree != NULL && !mTree->getPath(path, wd)) {
    return;
  }

  mQueue->enqueue(action, path, name);
}

void FanotifyService::dispatchRename(int fromWd, std::string fromName, int toWd, std::string toName) {
  std::string fromPath, toPath;

  if (mTree != NULL) {
    bool isFrom = mTree->getPath(fromPath, fromWd);
    bool isTo = mTree->getPath(toPath, toWd);

    if (!isFrom && !isTo) {
      return;
    } else if (!isFrom) {
      mQueue->enqueue(CREATED, toPath, toName);
      return;
    } else if (!isTo) {
      mQueue->enqueue(DELETED, fromPath, fromName);
      return;
    }
  }

  mQueue->enqueue(RENAMED, fromPath, fromName, toPath, toName);
}

std::string FanotifyService::getError() {
  if (!isWatching()) {
    return "Service shutdown unexpectedly";
  }

  return "";
}

bool FanotifyService::hasErrored() {
  return !isWatching();
}

bool FanotifyService::isWatching() {
  if (mEventLoop == NULL) {
    return false;
  }
  
  return mEventLoop->isLooping();
}

void FanotifyService::modify(int wd, std::string name) {
  dispatch(MODIFIED, wd, name);
}

void FanotifyService::remove(int wd, std::string name) {
  dispatch(DELETED, wd, name);
}

void FanotifyService::rename(int fromWd, std::string fromName, int toWd, std::string toName) {
  dispatchRename(fromWd, fromName, toWd, toName);
}

void FanotifyService::updateExcludedPaths(const std::vector<std::string> &excludedPaths) {
  mTree->updateExcludedPaths(excludedPaths);
}