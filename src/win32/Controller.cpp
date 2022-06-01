#include "../../includes/win32/Controller.h"

static bool isNtPath(const std::wstring &path) {
  return path.rfind(L"\\\\?\\", 0) == 0 || path.rfind(L"\\??\\", 0) == 0;
}

static std::wstring prefixWithNtPath(const std::wstring &path) {
  const ULONG widePathLength = GetFullPathNameW(path.c_str(), 0, nullptr, nullptr);
  if (widePathLength == 0) {
    return path;
  }

  std::wstring ntPathString;
  ntPathString.resize(widePathLength - 1);
  if (GetFullPathNameW(path.c_str(), widePathLength, &(ntPathString[0]), nullptr) != widePathLength - 1) {
    return path;
  }

  return ntPathString.rfind(L"\\\\", 0) == 0
    ? ntPathString.replace(0, 2, L"\\\\?\\UNC\\")
    : ntPathString.replace(0, 0, L"\\\\?\\");
}

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

Controller::Controller(std::shared_ptr<EventQueue> queue, const std::string &path, const std::vector<std::string> &excludedPaths) {
  auto widePath = convertMultiByteToWideChar(path);
  const bool isNt = isNtPath(widePath);
  if (!isNt) {
    // We convert to an NT Path to support paths > MAX_PATH
    widePath = prefixWithNtPath(widePath);
  }

  std::vector<std::wstring> excludedWidePaths;
  for (const std::string &path : excludedPaths) {
    std::wstring widePath = convertMultiByteToWideChar(path);
    const bool isNt = isNtPath(widePath);
    if (!isNt) {
      // We convert to an NT Path to support paths > MAX_PATH
      widePath = prefixWithNtPath(widePath);
    }
    excludedWidePaths.push_back(widePath);
  }

  mWatcher.reset(new Watcher(queue, widePath, isNt, excludedWidePaths));
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
    std::wstring widePath = convertMultiByteToWideChar(path);
    const bool isNt = isNtPath(widePath);
    if (!isNt) {
      // We convert to an NT Path to support paths > MAX_PATH
      widePath = prefixWithNtPath(widePath);
    }
    excludedWidePaths.push_back(widePath);
  }
  mWatcher->updateExcludedPaths(excludedWidePaths);
}
