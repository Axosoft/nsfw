#include "../includes/NativeInterface.h"

namespace {

// The Linux tree follows symlinks to directories, so it can report a path in the tree through one of them.
#if defined(__linux__) || defined(__FreeBSD__)
const bool FOLLOWS_DIRECTORY_LINKS = true;
#else
const bool FOLLOWS_DIRECTORY_LINKS = false;
#endif

}

NativeInterface::NativeInterface(const std::string &path, const std::vector<std::string> &excludedPaths,
  const FilterOptions &filterOptions, bool recursive, bool followFileSymlinks, std::shared_ptr<EventQueue> queue):
  mQueue(queue),
  mFilter(std::make_shared<PathFilter>(path, filterOptions)),
  // None for a watched file, the only watcher that isn't recursive: its patterns ignore every entry but the file's, so
  // nothing would ever be reported under a symlink, and each one found or created next to the file would be looked up,
  // and again on each change of the file it leads to, for nothing.
  mSymlinks(!recursive ? nullptr :
    std::make_shared<SymlinkIndex>(path, excludedPaths, mFilter->isCaseSensitive(), FOLLOWS_DIRECTORY_LINKS)) {
  mQueue->setSymlinks(mSymlinks);
#if defined(__linux__) || defined(__FreeBSD__)
  // Its scan already finds every symlink.
  (void)followFileSymlinks;
  mFilter->trackApplied();
  mQueue->setFilter(mFilter, true);
  // The Linux watcher also skips the directories it ignores, instead of watching them for nothing. It has no use for
  // recursive itself: the single file watcher excludes the subdirectories by patterns, so they cost no inotify watch.
  // It lists every directory it watches, so it tells the index, if there is one, the symlinks it finds there.
  mNativeInterface.reset(new NativeImplementation(queue, path, excludedPaths, mFilter, mSymlinks));
#elif defined(_WIN32) || defined(__APPLE_CC__)
  mQueue->setFilter(mFilter);
  // These watchers skip the directories the filter ignores when they report what a directory that appeared holds.
  mNativeInterface.reset(new NativeImplementation(queue, path, excludedPaths, mFilter, recursive));
  // After the watcher started, so a symlink created during the walk is followed from its event if not found by it.
  if (mSymlinks && followFileSymlinks && mNativeInterface->isWatching()) {
    std::shared_ptr<PathFilter> filter = mFilter;
    mSymlinks->walk([filter](const std::string &directory, const std::string &name) {
      return filter->isActive() && filter->isIgnored(directory, name, PathFilter::Kind::DIRECTORY);
    });
  }
#else
  (void)followFileSymlinks;
  mQueue->setFilter(mFilter);
  mNativeInterface.reset(new NativeImplementation(queue, path, excludedPaths, recursive));
#endif
}

NativeInterface::~NativeInterface() {
  if (mSymlinks) mSymlinks->stopWalk();
  mNativeInterface.reset(nullptr);
  mQueue->setFilter(nullptr);
  mQueue->setSymlinks(nullptr);
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
  if (mSymlinks) mSymlinks->setExcludedPaths(excludedPaths);
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
  // What changed before this call is judged by the patterns in force then, see InotifyService::updateExcludedPatterns().
  mNativeInterface->flush();
  mFilter->setPatterns(excludedPatterns);
  mFilter->commitPatterns();
#endif
}
