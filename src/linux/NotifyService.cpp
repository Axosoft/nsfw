#include "../../includes/linux/NotifyService.h"

NotifyService::NotifyService(std::shared_ptr<EventQueue> queue, std::string path, const std::vector<std::string> &excludedPaths) {
  mFanotifyService = new FanotifyService(queue, path, excludedPaths);

  if (!mFanotifyService->isWatching()) {
    delete mFanotifyService;
    mInotifyService = new InotifyService(queue, path, excludedPaths);
  }
}

NotifyService::~NotifyService() {
  if (mFanotifyService != NULL) {
    delete mFanotifyService;
  }

  if (mInotifyService != NULL) {
    delete mInotifyService;
  }
}

std::string NotifyService::getError() {
  if (mInotifyService != NULL) {
    return mInotifyService->getError();
  }

  if (mFanotifyService != NULL) {
    return mFanotifyService->getError();
  }

  return "";
}

bool NotifyService::hasErrored() {
  if (mInotifyService != NULL) {
    return mInotifyService->hasErrored();
  }

  if (mFanotifyService != NULL) {
    return mFanotifyService->hasErrored();
  }

  return false;
}

bool NotifyService::isWatching() {
  if (mInotifyService != NULL) {
    return mInotifyService->isWatching();
  }

  if (mFanotifyService != NULL) {
    return mFanotifyService->isWatching();
  }

  return false;
}

void NotifyService::updateExcludedPaths(const std::vector<std::string> &excludedPaths) {
  if (mInotifyService != NULL) {
    mInotifyService->updateExcludedPaths(excludedPaths);
  }

  if (mFanotifyService != NULL) {
    mFanotifyService->updateExcludedPaths(excludedPaths);
  }
}