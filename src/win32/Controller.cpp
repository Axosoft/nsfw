#include "../../includes/win32/Controller.h"

static std::wstring convertMultiByteToWideChar(const std::string &multiByte) {
  const int wlen = MultiByteToWideChar(CP_UTF8, 0, multiByte.data(), -1, 0, 0);

  if (wlen == 0) {
    return std::wstring();
  }

  std::wstring wideString;
  wideString.resize(wlen - 1);

  int failureToResolveUTF8 = MultiByteToWideChar(CP_UTF8, 0, multiByte.data(), -1, &(wideString[0]), wlen);
  if (failureToResolveUTF8 == 0) {
    return std::wstring();
  }

  return wideString;
}

Controller::Controller(std::shared_ptr<EventQueue> queue, const std::string &path,
    const std::vector<std::string> &excludedPaths, std::shared_ptr<PathFilter> filter, bool recursive) {
  auto widePath = convertMultiByteToWideChar(path);
  std::vector<std::wstring> excludedWidePaths;
  for (const std::string &path : excludedPaths) {
    excludedWidePaths.push_back(convertMultiByteToWideChar(path));
  }

  mWatcher.reset(new Watcher(queue, widePath, excludedWidePaths, filter, recursive));
}

Controller::~Controller() {
  mWatcher.reset();
}

std::string Controller::getError() {
  return mWatcher->getError();
}

bool Controller::hasErrored() {
  return !mWatcher->getError().empty();
}

bool Controller::isWatching() {
  return !hasErrored() && mWatcher->isRunning();
}

void Controller::updateExcludedPaths(const std::vector<std::string> &excludedPaths) {
  std::vector<std::wstring> excludedWidePaths;
  for (const std::string &path : excludedPaths) {
    excludedWidePaths.push_back(convertMultiByteToWideChar(path));
  }
  mWatcher->updateExcludedPaths(excludedWidePaths);
}
