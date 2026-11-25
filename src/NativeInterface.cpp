#include "../includes/NativeInterface.h"

NativeInterface::NativeInterface(const std::string &path, const std::vector<std::string> &excludedPaths,
  const FilterOptions &filterOptions, bool recursive, std::shared_ptr<EventQueue> queue):
  mQueue(queue),
  mFilter(std::make_shared<PathFilter>(path, filterOptions)) {
#if defined(__linux__) || defined(__FreeBSD__)
  mFilter->trackApplied();
  mQueue->setFilter(mFilter, true);
  // The Linux watcher also skips the directories it ignores, instead of watching them for nothing. It has no use for
  // recursive: the single file watcher excludes the subdirectories by patterns, so they cost no inotify watch.
  (void)recursive;
  mNativeInterface.reset(new NativeImplementation(queue, path, excludedPaths, mFilter));
#elif defined(_WIN32) || defined(__APPLE_CC__)
  mQueue->setFilter(mFilter);
  // These watchers skip the directories the filter ignores when they report what a directory that appeared holds.
  mNativeInterface.reset(new NativeImplementation(queue, path, excludedPaths, mFilter, recursive));
#else
  mQueue->setFilter(mFilter);
  mNativeInterface.reset(new NativeImplementation(queue, path, excludedPaths, recursive));
#endif
}

NativeInterface::~NativeInterface() {
  mNativeInterface.reset(nullptr);
  mQueue->setFilter(nullptr);
}

std::string NativeInterface::getError() {
  return mNativeInterface->getError();
}

bool NativeInterface::hasErrored() {
  return mNativeInterface->hasErrored();
}

bool NativeInterface::isWatching() {
  return mNativeInterface->isWatching();
}

std::future<bool> NativeInterface::updateExcludedPaths(const std::vector<std::string> &excludedPaths) {
#if defined(__linux__) || defined(__FreeBSD__)
  return mNativeInterface->updateExcludedPaths(excludedPaths);
#else
  mNativeInterface->updateExcludedPaths(excludedPaths);
  return {};
#endif
}

// The other watchers only report through the queue, which asks the filter about every event.
void NativeInterface::updateExcludedPatterns(const std::vector<std::string> &excludedPatterns) {
#if defined(__linux__) || defined(__FreeBSD__)
  mNativeInterface->updateExcludedPatterns(excludedPatterns);
#else
  mFilter->setPatterns(excludedPatterns);
  mFilter->commitPatterns();
#endif
}
