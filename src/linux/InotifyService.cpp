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

InotifyService::InotifyService(std::shared_ptr<EventQueue> queue, std::string path,
  const std::vector<std::string> &excludedPaths, std::shared_ptr<PathFilter> filter):
  mEventLoop(NULL),
  mQueue(queue),
  mTree(NULL),
  mFilter(filter) {
  mInotifyInstance = inotify_init();

  if (mInotifyInstance == -1) {
    return;
  }

  const uint64_t beforeScan = mFilter->generation();
  mTree = new InotifyTree(mInotifyInstance, path, excludedPaths, mFilter);
  // The scan used the rules as they are. One that changed meanwhile (read again after failing) may have decided some
  // directories by other rules: it stays to commit, and the event loop applies it then, like any change.
  mCommittedGeneration = mExcludedGeneration = mSeenGeneration = mFilter->commit(beforeScan);
  if (mFilter->generation() == beforeScan) {
    takeFilterChanges();
    mPendingDirectories.clear();
    mPendingAll = false;
  }
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

// Not known to be a file: a symlink to a directory counts as one, as the tree follows it, see PathFilter::kindOf().
void InotifyService::create(int wd, std::string name, bool fromInCreate) {
  dispatch(CREATED, wd, name, fromInCreate);
}

// Tells the filter about every entry change before the tree acts on it, whether or not it is reported: the queue only
// sees the reported ones, so it leaves this to the watcher, see NativeInterface. In a directory held at a symlink,
// also by its physical path, which the filter keeps rules under too, see InotifyTree::physicalPathOf().
void InotifyService::noteEntry(int wd, const std::string &directory, const std::string &name, bool replaced) {
  if (!mFilter->mayChangeRules(name, replaced)) {
    // Patterns alone: only what was seen below it, see PathFilter::seenMoved().
    if (replaced && mFilter->isActive()) mFilter->entryChanged(directory, name, true);
    return;
  }
  mFilter->entryChanged(directory, name, replaced);
  std::string physical;
  if (mTree->physicalPathOf(wd, physical)) {
    mFilter->entryChanged(physical, name, replaced);
  }
}

void InotifyService::noteMove(int fromWd, const std::string &fromPath, const std::string &fromName, int toWd,
  const std::string &toPath, const std::string &toName) {
  // Any moved entry may carry the rules read below it, so only whether ignore files are followed at all matters.
  if (!mFilter->mayChangeRules(fromName, true)) {
    // Patterns alone: only what was seen below it, see PathFilter::seenMoved().
    if (mFilter->isActive()) mFilter->entryMoved(fromPath, fromName, toPath, toName);
    return;
  }
  mFilter->entryMoved(fromPath, fromName, toPath, toName);
  std::string fromPhysical, toPhysical;
  if (mTree->physicalPathOf(fromWd, fromPhysical)) {
    mFilter->entryChanged(fromPhysical, fromName, true);
  }
  if (mTree->physicalPathOf(toWd, toPhysical)) {
    mFilter->entryChanged(toPhysical, toName, true);
  }
}

static int millisecondsUntil(std::chrono::steady_clock::time_point time, std::chrono::steady_clock::time_point now) {
  return time <= now ? 0 : (int)std::chrono::ceil<std::chrono::milliseconds>(time - now).count();
}

static int sooner(int wait, int other) {
  return wait < 0 ? other : std::min(wait, other);
}

// Has the tree follow the ignore files: drop what they ignore once they stayed unchanged for EXCLUSION_QUIET_PERIOD,
// and watch what they no longer ignore once they stayed so for INCLUSION_QUIET_PERIOD, see the constants. Ignore files
// that couldn't be read are read again every FAILED_READ_RETRY, see PathFilter::retryFailedReads(). Returns how many
// milliseconds to wait before asking again, -1 when nothing is left to do. Called by the event loop with event
// handling locked, with no move pending.
int InotifyService::settleFilterChange() {
  const auto now = std::chrono::steady_clock::now();
  int wait = -1;
  if (mFilter->hasFailedReads()) {
    if (now >= mNextFailedReadRetry) {
      mFilter->retryFailedReads();
      mNextFailedReadRetry = now + FAILED_READ_RETRY;
    }
    if (mFilter->hasFailedReads()) {
      wait = millisecondsUntil(mNextFailedReadRetry, now);
    }
  }
  const uint64_t generation = mFilter->generation();
  if (generation == mCommittedGeneration) {
    return wait;
  }
  if (generation != mSeenGeneration) {
    if (mSeenGeneration == mCommittedGeneration) {
      mFirstUnapplied = now;
    }
    mSeenGeneration = generation;
    mLastFilterChange = now;
  }
  const auto excludeAt = std::min(mLastFilterChange + EXCLUSION_QUIET_PERIOD, mFirstUnapplied + EXCLUSION_MAX_DELAY);
  const auto includeAt = std::min(mLastFilterChange + INCLUSION_QUIET_PERIOD, mFirstUnapplied + INCLUSION_MAX_DELAY);
  if (now >= includeAt) {
    applyFilter(true);
    return mFilter->generation() == mCommittedGeneration ? wait : 0;
  }
  // Spaced too once the max delay passed, so a burst of changes doesn't have the tree walked at every read.
  if (now >= excludeAt && now >= mLastExclusionPass + EXCLUSION_QUIET_PERIOD && mExcludedGeneration != generation) {
    applyFilter(false);
  }
  const auto nextExclusion = std::max(excludeAt, mLastExclusionPass + EXCLUSION_QUIET_PERIOD);
  return sooner(wait, millisecondsUntil(mExcludedGeneration == generation ? includeAt : nextExclusion, now));
}

void InotifyService::takeFilterChanges() {
  std::vector<std::string> directories;
  bool all = false;
  mFilter->takeChanges(directories, all);
  mPendingAll = mPendingAll || all;
  mPendingDirectories.insert(directories.begin(), directories.end());
}

// Applies the filter's answers: what it ignores below the directories whose ignore files changed is dropped. With
// include, the rules in force become the applied ones, and what neither ignores is watched again, wherever it is.
// Until then the tree never watches what either the applied rules or the ones in force ignore, see
// PathFilter::isChildIgnored(), whatever asks it.
void InotifyService::applyFilter(bool include) {
  // Only what was taken here is excluded, so only that is committed: a change the pass itself reads (an ignore file
  // read again after failing, say) waits for the next one.
  const uint64_t taken = mFilter->generation();
  takeFilterChanges();
  const std::vector<std::string> directories = mPendingAll ? std::vector<std::string> { "" } :
    std::vector<std::string>(mPendingDirectories.begin(), mPendingDirectories.end());
  mTree->excludeFiltered(directories, emitCreated());
  // Walked once: the next pass walks only what changes after this one.
  mPendingDirectories.clear();
  mPendingAll = false;
  mExcludedGeneration = taken;
  mLastExclusionPass = std::chrono::steady_clock::now();
  if (include) {
    mCommittedGeneration = mSeenGeneration = mFilter->commit(taken);
    mTree->includeFiltered();
  }
  finishFilterPass();
}

void InotifyService::finishFilterPass() {
  mTree->removeDroppedWatches(0);
  // What rejecting and including names changed for the ways of the excluded paths and the symlinks, without waiting
  // for an event.
  revalidateRejected();
  revalidateLinks();
}

// fromInCreate: a creation that is an IN_CREATE, not a move in, see InotifyTree::isReportedByScan().
// kind: what inotify told the entry is, which the filter can't look up once it is removed.
void InotifyService::dispatch(EventType action, int wd, std::string name, bool fromInCreate, PathFilter::Kind kind) {
  std::string path;
  if (!mTree->getPath(path, wd)) {
    return;
  }
  // A creation replaces nothing: a removal or a move away forgot what was at that path. And a late IN_CREATE of a
  // directory the tree already scanned mustn't make the filter forget the rules that scan used.
  noteEntry(wd, path, name, action == DELETED);

  bool reportedByScan = fromInCreate && mTree->isReportedByScan(wd, name);
  if (!mTree->isRootExcluded() && !reportedByScan) mQueue->enqueue(action, path, name, "", "", kind);
  // Reported first: the tree change may report creations, see renameDirectory().
  if (action != MODIFIED) {
    mTree->entryChanged(wd, name, action == DELETED, emitCreated());
  }
}

void InotifyService::dispatchRename(int fromWd, std::string fromName, int toWd, std::string toName) {
  std::string fromPath, toPath;
  if (!mTree->getPath(fromPath, fromWd) || !mTree->getPath(toPath, toWd)) {
    return;
  }
  noteMove(fromWd, fromPath, fromName, toWd, toPath, toName);

  if (!mTree->isRootExcluded()) mQueue->enqueue(RENAMED, fromPath, fromName, toPath, toName);
  mTree->entryChanged(fromWd, fromName, true, emitCreated());
  mTree->entryChanged(toWd, toName, false, emitCreated());
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

// A symlink the tree held, skipped or rejected as a directory was one too, for the filter, which followed it when its
// creation was reported; the filter only falls back on this for one it never saw, see PathFilter::judge().
void InotifyService::remove(int wd, std::string name, bool isDirectory) {
  const bool directory = isDirectory || (mFilter->isActive() && mTree->hadDirectoryAt(wd, name));
  dispatch(DELETED, wd, name, false, directory ? PathFilter::Kind::DIRECTORY : PathFilter::Kind::FILE);
}

// Either side of a move may be a directory that was dropped from the tree (moved out, into an excluded directory, or
// excluded) whose watch the kernel still has: its events queued before the drop, or the watch isn't removed yet (see
// InotifyTree::removeDroppedWatches()). The move is then only a move in or out.
void InotifyService::rename(int fromWd, std::string fromName, int toWd, std::string toName) {
  if (!mTree->nodeExists(fromWd)) {
    create(toWd, toName, false);
  } else if (!mTree->nodeExists(toWd)) {
    remove(fromWd, fromName, false);
  } else {
    dispatchRename(fromWd, fromName, toWd, toName);
  }
}

void InotifyService::createDirectory(int wd, std::string name, bool fromInCreate) {
  if (!mTree->nodeExists(wd)) {
    return;
  }

  dispatch(CREATED, wd, name, fromInCreate, PathFilter::Kind::DIRECTORY);
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

void InotifyService::startRead(size_t bytesRead) {
  mTree->startRead(bytesRead);
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

// The scans of an update, on another thread than the event loop's, can read an ignore file that changed: the loop then
// has a generation to apply, which it may be waiting for an event to notice.
void InotifyService::wakeForFilterChange() {
  if (mFilter->generation() != mCommittedGeneration || mFilter->hasFailedReads()) {
    mEventLoop->wake();
  }
}

// Once a read is handled, see InotifyEventLoop::work().
void InotifyService::revalidateRejected() {
  // Once per read, not per event: a tree full of symlinks can change many in one.
  mTree->recheckFilteredLinks(emitCreated());
  mTree->revalidateRejected(emitCreated());
}

void InotifyService::revalidateLinks(bool unknownMoveDestination) {
  mTree->revalidateLinks(unknownMoveDestination, emitCreated());
}

void InotifyService::forgetIncompleteScans() {
  mTree->forgetIncompleteScans();
}

void InotifyService::removeMovedDirectory(int wd, std::string name) {
  dispatch(DELETED, wd, name, false, PathFilter::Kind::DIRECTORY);
  mTree->removeMovedDirectory(wd, name, emitCreated());
  revalidateLinks(true);
}

// See rename() for a side that isn't in the tree.
void InotifyService::renameDirectory(int fromWd, std::string fromName, int toWd, std::string toName) {
  if (!mTree->nodeExists(toWd)) {
    removeMovedDirectory(fromWd, fromName);
    return;
  }
  if (!mTree->nodeExists(fromWd)) {
    createDirectory(toWd, toName, false);
    return;
  }

  std::string fromPath, toPath;
  if (mTree->getPath(fromPath, fromWd) && mTree->getPath(toPath, toWd)) {
    // Before the tree moves the node: the rules the tree applied below it move along, see PathFilter::entryMoved().
    noteMove(fromWd, fromPath, fromName, toWd, toPath, toName);
  }
  // Reported once the tree has the destination at its own path (see InotifyTree::untangleMove()), and before it drops
  // the node of the directory it left (see InotifyTree::discard()) or reports creations under the new path.
  bool kept = mTree->renameDirectory(fromWd, fromName, toWd, toName, emitCreated(),
    [this, fromWd, &fromName, toWd, &toName](bool renamed) {
      std::string fromPath, toPath;
      if (mTree->isRootExcluded() || !mTree->getPath(fromPath, fromWd)) {
        return;
      }
      if (!renamed) {
        mQueue->enqueue(DELETED, fromPath, fromName, "", "", PathFilter::Kind::DIRECTORY);
      } else if (mTree->getPath(toPath, toWd)) {
        mQueue->enqueue(RENAMED, fromPath, fromName, toPath, toName, PathFilter::Kind::DIRECTORY);
      }
    });
  if (!kept) {
    revalidateLinks(true);
  }
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
  wakeForFilterChange();
  return completion;
}

void InotifyService::updateExcludedPatterns(const std::vector<std::string> &excludedPatterns) {
  // Like updateExcludedPaths(): the filter's answers change for the tree the event loop thread reads and changes. A
  // directory found again whose scan is pending has no children yet: that scan asks the filter as it is then.
  std::unique_lock<std::mutex> lock = mEventLoop->lockEventHandling();
  // Patterns apply anywhere, at once: what they exclude now is dropped, and what they no longer exclude is watched,
  // unless an ignore file change still waiting for its quiet period ignores it, which the event loop applies later.
  mFilter->setPatterns(excludedPatterns);
  mTree->excludeFiltered({ "" }, emitCreated());
  mFilter->commitPatterns();
  mTree->includeFiltered();
  finishFilterPass();
  mEventLoop->wakeForEntryRetry();
  wakeForFilterChange();
}
