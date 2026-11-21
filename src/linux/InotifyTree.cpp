#include "../../includes/linux/InotifyTree.h"
#include <cstdio>
#include <sys/stat.h>
#include <poll.h>
#include <sys/ioctl.h>
#include <climits>
#include <unistd.h>

#include <algorithm>

static const char *const ROOT_CHANGED_ERROR = "Service shutdown: root path changed (renamed or deleted)";

/**
 * InotifyTree ---------------------------------------------------------------------------------------------------------
 */
// Whether path (or the symlink at it, when throughSymlink) leads to the directory with inodeNumber on device.
static bool leadsTo(const std::string &path, bool throughSymlink, dev_t device, ino_t inodeNumber) {
  struct stat file;
  int result = throughSymlink ? stat(path.c_str(), &file) : lstat(path.c_str(), &file);
  return result == 0 && S_ISDIR(file.st_mode) && file.st_dev == device && file.st_ino == inodeNumber;
}

InotifyTree::InotifyTree(int inotifyInstance, std::string path, const std::vector<std::string> &excludedPaths):
  mError(""),
  mInotifyInstance(inotifyInstance),
  mRoot(NULL) {
  mInotifyNodeByWatchDescriptor = new std::map<int, InotifyNode *>;
  // The default, where /proc can't tell.
  mMaxQueuedEvents = 16384;
  FILE *maxQueuedEvents = fopen("/proc/sys/fs/inotify/max_queued_events", "r");
  if (maxQueuedEvents != NULL) {
    int value;
    if (fscanf(maxQueuedEvents, "%d", &value) == 1 && value > 0) {
      mMaxQueuedEvents = value;
    }
    fclose(maxQueuedEvents);
  }
  std::string directory;
  std::string watchName;
  if (path.length() == 1 && path[0] == '/') {
    directory = "/";
    watchName = "";
  } else {
    uint32_t location = path.find_last_of("/");
    directory = (location == 0) ? "" : path.substr(0, location);
    watchName = path.substr(location + 1);
  }

  mWatchedPath = path;
#ifdef NSFW_TEST_SLOW_1
  if (access((path + ".nsfw-test-one-marker").c_str(), F_OK) == 0) {
    mMarkerBatch = mMarkerCredit = 1;
  }
#endif

  struct stat file;
  if (stat(mWatchedPath.c_str(), &file) < 0) {
    return;
  }
  mWatchedDirectory = { file.st_dev, file.st_ino };
  setExcludedPaths(excludedPaths);
  mRootExcluded = std::find(mExcludedPaths.begin(), mExcludedPaths.end(), mWatchedPath) != mExcludedPaths.end();

  InotifyNode *root = new InotifyNode(
    this,
    mInotifyInstance,
    NULL,
    directory,
    watchName,
    file.st_dev,
    file.st_ino,
    true
  );
  addInode(file.st_ino, root);

  if (!root->isAlive()) {
    delete root;
    return;
  }
  // Nothing moved during the initial scan has events coming: a directory seen twice is seen through a symlink or a
  // mount. Where the tree has it at the symlink, its own place is kept, for its node to take once the symlink no longer
  // has it, see takeOwnPlace().
  for (auto sighting = mSightings.begin(); sighting != mSightings.end();) {
    InotifyNode *holder = findNodeByWatchDescriptor(sighting->first);
    bool kept = holder != NULL && holder->isThroughSymlink() && !sighting->second.throughSymlink;
    int watchDescriptor = (sighting++)->first;
    if (!kept) {
      eraseSighting(watchDescriptor);
    }
  }
  mRoot = root;
  // With every node there: what an excluded path named or didn't while the scan ran, by a way that led elsewhere
  // once the directories on it were watched, see followWay().
  revalidateRejected(nullptr);
  excludeNamed(root, mExcludedNames, nullptr);
}

bool InotifyTree::existWatchedPath() {
  struct stat file;
  return stat(mWatchedPath.c_str(), &file) >= 0;
}

bool InotifyTree::isWatchedPathGone() {
  struct stat file;
  return stat(mWatchedPath.c_str(), &file) != 0 && (errno == ENOENT || errno == ENOTDIR);
}

void InotifyTree::addDirectory(int wd, std::string name, EmitCreatedEvent emitCreatedEvent) {
  auto nodeIterator = mInotifyNodeByWatchDescriptor->find(wd);
  if (nodeIterator == mInotifyNodeByWatchDescriptor->end()) {
    return;
  }

  InotifyNode *node = nodeIterator->second;
  bool gone = node->createChild(name, emitCreatedEvent);
  if (gone) {
    // Gone before its event was handled; a rename of this directory may have taken it, see rescanIncomplete().
    markScanIncomplete(node);
  }
}

void InotifyTree::addNodeReferenceByWD(int wd, InotifyNode *node) {
  (*mInotifyNodeByWatchDescriptor)[wd] = node;
}

std::string InotifyTree::getError() {
  // A watched path that is gone explains the failure better than what was recorded first: restarting won't help, and
  // an overflow may have dropped the root's own deletion events. Only when it's really gone, though: a stat() that
  // fails for another reason (permissions, a network filesystem hiccup) mustn't hide the recorded cause.
  if (isWatchedPathGone()) {
    return ROOT_CHANGED_ERROR;
  }

  std::lock_guard<std::mutex> lock(mErrorMutex);
  return mError;
}

bool InotifyTree::getPath(std::string &out, int wd) {
  auto nodeIterator = mInotifyNodeByWatchDescriptor->find(wd);
  if (nodeIterator == mInotifyNodeByWatchDescriptor->end()) {
    return false;
  }

  out = nodeIterator->second->getFullPath();
  return true;
}

bool InotifyTree::isRootExcluded() const {
  return mRootExcluded;
}

bool InotifyTree::hasErrored() {
  {
    std::lock_guard<std::mutex> lock(mErrorMutex);
    if (!mError.empty()) return true;
  }

  // stat() outside the lock, so a slow filesystem doesn't hold up the other threads' error checks.
  if (existWatchedPath()) {
    return false;
  }

  setError(ROOT_CHANGED_ERROR);
  return true;
}

bool InotifyTree::isRootAlive() {
  return mRoot != NULL;
}

bool InotifyTree::nodeExists(int wd) {
  auto nodeIterator = mInotifyNodeByWatchDescriptor->find(wd);
  return nodeIterator != mInotifyNodeByWatchDescriptor->end();
}

void InotifyTree::removeDirectory(int wd, EmitCreatedEvent emitCreatedEvent) {
  auto nodeIterator = mInotifyNodeByWatchDescriptor->find(wd);
  if (nodeIterator != mInotifyNodeByWatchDescriptor->end()) {
    InotifyNode *node = nodeIterator->second;
    InotifyNode *parent = node->getParent();
    if (parent == NULL) {
      // The error goes first, so the poll thread never finds the root gone without it and reports a generic one.
      setError(ROOT_CHANGED_ERROR);
      delete mRoot.exchange(NULL);
    } else {
      // The directories it held at symlinks aren't deleted with it.
      DiscardSteps steps;
      keepSightedBelow(node, Departure::LEFT, steps);
      // A directory that is deleted wasn't the one a scan saw elsewhere: that one got its inode number.
      auto found = mSightings.find(wd);
      if (found != mSightings.end()) {
        steps.reborn.push_back(found->second);
      }
      parent->removeChild(node->getName());
      // The kernel removed its watch, or is removing it: there's none left to remove, and its IN_IGNORED may be read
      // already.
      mDroppedWatches.erase(wd);
      settleKept(steps, emitCreatedEvent);
    }
  }
}

// The directory `name` left the directory with watch descriptor wd for a place its watches can't follow: outside the
// watched path, or into an excluded directory. Its nodes would keep reporting its events under the old path, and keep
// its inode claimed, so a directory moved back in later would not be watched.
void InotifyTree::removeMovedDirectory(int wd, std::string name, EmitCreatedEvent emitCreatedEvent) {
  auto nodeIterator = mInotifyNodeByWatchDescriptor->find(wd);
  if (nodeIterator == mInotifyNodeByWatchDescriptor->end()) {
    return;
  }

  InotifyNode *child = nodeIterator->second->findDescendant(name);
  if (child != NULL) {
    discard(child, emitCreatedEvent);
    return;
  }
  // The tree may have it at a symlink scanned before it, which led to it by this path and leads nowhere now.
  InotifyNode *holder = findSightedAt(wd, name);
  if (holder != NULL && holder->isAtStaleLink()) {
    discard(holder, emitCreatedEvent);
  }
}

// How many bytes of events are waiting to be read, INT_MAX if some are but it can't be told how many.
int InotifyTree::getQueuedBytes() {
  int queuedBytes;
  if (ioctl(mInotifyInstance, FIONREAD, &queuedBytes) == 0) {
    return queuedBytes;
  }
  // Where FIONREAD isn't supported. Unlike ioctl(), poll() is a cancellation point; with the lock held, the unwinding
  // releases it.
  pollfd queued = { mInotifyInstance, POLLIN, 0 };
  int ready;
  do {
    ready = poll(&queued, 1, 0);
  } while (ready == -1 && errno == EINTR);
  if (ready < 0 || (queued.revents & (POLLERR | POLLHUP | POLLNVAL))) {
    setError("Cannot inspect inotify event queue");
  }
  return ready == 0 ? 0 : INT_MAX;
}

// Removes the kernel watches of the directories dropped from the tree. Each removal queues an IN_IGNORED event, and
// removing thousands at once would overflow the inotify queue, whose limit counts events, so no more go than half the
// eventsRead the event loop just took from it and didn't spend on removeDroppedWatch() (a small batch more when nothing
// is queued), and only while at most a read waits: the queue still shrinks. Their own IN_IGNORED events wake the event
// loop up, which calls this again after each read. Busy excluded directories' watches go at their next event anyway,
// see removeDroppedWatch(), and the others only take room in fs.inotify.max_user_watches meanwhile, see
// freeDroppedWatch().
void InotifyTree::removeDroppedWatches(int eventsRead) {
  if (mDroppedWatches.empty() || mDeferred) return;
  int queuedBytes = getQueuedBytes();
  if (queuedBytes <= READ_SIZE) {
    int budget = std::min(eventsRead / 2 + (queuedBytes == 0 ? DROPPED_WATCHES_PER_BATCH : 0), roomInQueue(queuedBytes));
    budget = std::min(budget, mMarkerCredit);
    for (int removed = 0; removed < budget && !mDroppedWatches.empty();) {
      if (removeWatch(mDroppedWatches.begin()->first, 0)) {
        ++removed;
        --mMarkerCredit;
      }
    }
  }
}

// After a read, with no move pending: watches the directories that couldn't be watched for lack of room, see
// InotifyNode::inotifyInit(), and the ones a scan saw elsewhere whose node went when its watch made a marker, once it's
// read, see releaseSighted(). A move pending may be one of them going to a watched path, where its other half adds a
// node for it, and a retry before that would watch it at the sighted place instead. What they hold then is reported as
// created, also for one that updateExcludedPaths() included again, whose creation isn't otherwise reported: what was
// created in it until now has no event of its own.
void InotifyTree::retryWatches(EmitCreatedEvent emitCreatedEvent) {
  std::vector<Sighting> retries;
  retries.swap(mRetries);
  for (const Sighting &sighting : retries) {
    watchSighted(sighting, emitCreatedEvent);
  }
}

// How many more events the queue takes, less a margin for other processes' events (a quarter of the queue at most, for
// a small one): FIONREAD over the smallest event size counts the queued ones at most.
int InotifyTree::roomInQueue(int queuedBytes) {
  if (queuedBytes == INT_MAX) {
    return 0;
  }
  int margin = std::min(QUEUE_MARGIN, mMaxQueuedEvents / 4);
  return std::max(0, mMaxQueuedEvents - queuedBytes / (int)sizeof(struct inotify_event) - margin);
}

// Removes a dropped watch when the limit of watches is reached: they still count against it. An excluded directory's
// first if one is among the first few, since one that moved away may come back and take its watch back (behind the
// marker of its pass, see InotifyNode::inotifyInit()). Only while the queue is less than half full, so that a burst of
// directories created at the limit doesn't fill it with the IN_IGNORED of a watch each, besides their own events.
// Whether one was removed.
bool InotifyTree::freeDroppedWatch() {
  if (mDroppedWatches.empty() || roomInQueue(getQueuedBytes()) <= mMaxQueuedEvents / 2) {
    return false;
  }
  int wd = mDroppedWatches.begin()->first;
  int looked = 0;
  for (auto dropped = mDroppedWatches.begin(); dropped != mDroppedWatches.end() && looked < DROPPED_WATCHES_PER_BATCH;
       ++dropped, ++looked) {
    if (dropped->second.excluded) {
      wd = dropped->first;
      break;
    }
  }
  removeWatch(wd, 0);
  return true;
}

// A removed watch keeps its marker until IN_IGNORED, which arrives after the events queued for that watch.
bool InotifyTree::removeWatch(int wd, uint64_t marker) {
  auto dropped = mDroppedWatches.find(wd);
  if (dropped == mDroppedWatches.end()) {
    return false;
  }
  mDroppedWatches.erase(dropped);
  bool queued = inotify_rm_watch(mInotifyInstance, wd) == 0;
  mRemovedWatches[wd] = { queued ? ++mLastRemoval : 0, queued ? marker : 0 };
  return queued;
}

// An excluded directory whose watch still has events: its watch goes now, for one IN_IGNORED instead of all its events.
// Whether that IN_IGNORED was queued. Not one that moved away: it may come back, and keep its watch.
bool InotifyTree::removeDroppedWatch(int wd) {
  auto dropped = mDroppedWatches.find(wd);
  if (dropped == mDroppedWatches.end() || !dropped->second.excluded) {
    return false;
  }
  return removeWatch(wd, 0);
}

void InotifyTree::removeNodeReferenceByWD(int wd, InotifyNode *node) {
  auto nodeIterator = mInotifyNodeByWatchDescriptor->find(wd);
  if (nodeIterator != mInotifyNodeByWatchDescriptor->end() && nodeIterator->second == node) {
    mInotifyNodeByWatchDescriptor->erase(nodeIterator);
    // Its events are dropped from now on. The watch itself goes later, see mDroppedWatches.
    mDroppedWatches[wd] = { mExcluding };
    eraseSighting(wd);
    mQuarantined.erase(wd);
    directoryChanged({ node->getDevice(), node->getInodeNumber() }, NULL);
  }
}

void InotifyTree::renameDirectory(
  int fromWd,
  std::string fromName,
  int toWd,
  std::string toName,
  EmitCreatedEvent emitCreatedEvent
) {
  auto fromNodeIterator = mInotifyNodeByWatchDescriptor->find(fromWd);
  auto toNodeIterator = mInotifyNodeByWatchDescriptor->find(toWd);
  if (fromNodeIterator == mInotifyNodeByWatchDescriptor->end() || toNodeIterator == mInotifyNodeByWatchDescriptor->end()) {
    return;
  }

  InotifyNode *fromNode = fromNodeIterator->second;
  InotifyNode *toNode = toNodeIterator->second;
  InotifyNode *child = fromNode->findDescendant(fromName);
  if (!child) {
    // Where a scan saw it, if the tree has it at a symlink: that place moves with it, for the node to take or, if an
    // excluded path names it, to go, see settleLink(). Before anything else, so that a later move of it from there
    // finds it, see removeMovedDirectory().
    InotifyNode *sighted = findSightedAt(fromWd, fromName);
    if (sighted != NULL) {
      moveSighting(sighted->getWatchDescriptor(), toWd, toName);
    }
    // It wasn't watched where it was, most likely because that path is excluded, so what it holds wasn't reported.
    bool gone = toNode->createChild(toName, emitCreatedEvent);
    if (gone) {
      markScanIncomplete(toNode);
    }
    // Or the tree has it at a symlink scanned before it, while its own path is the one that changed: the symlink may
    // not lead to it anymore, and what the excluded paths name below it may have changed too. Watched only now, it may
    // be where such symlinks led below it.
    struct stat file;
    InotifyNode *holder = NULL;
    if (
      toNode->leadsToItself() &&
      lstat(InotifyNode::createFullPath(toNode->getFullPath(), toName).c_str(), &file) == 0
    ) {
      holder = findNodeOf(file);
    }
    if (holder == NULL || holder->getParent() == NULL) {
      return;
    }
    if (!holder->isThroughSymlink()) {
      settleLinksTo(holder, emitCreatedEvent);
    } else if (holder->leadsToItself()) {
      applyExclusions(holder, emitCreatedEvent);
    } else {
      settleLink(holder, emitCreatedEvent);
    }
    return;
  }

  int childWatchDescriptor = child->getWatchDescriptor();
  if (isAncestorOrSelf(child, toNode)) {
    // It moved into a directory the tree only has below it, at a symlink it holds: that one takes its own place
    // first, since nothing can hang from itself. With none to take, the directory left for a place that isn't watched.
    InotifyNode *holder = toNode;
    while (holder != child && !holder->isThroughSymlink()) {
      holder = holder->getParent();
    }
    bool untangled = holder != child && moveToOwnPlace(holder, emitCreatedEvent) == OwnPlace::TAKEN;
    fromNode = findNodeByWatchDescriptor(fromWd);
    toNode = findNodeByWatchDescriptor(toWd);
    child = fromNode == NULL ? NULL : fromNode->findDescendant(fromName);
    if (child == NULL || child->getWatchDescriptor() != childWatchDescriptor) {
      return;
    }
    if (!untangled || toNode == NULL || isAncestorOrSelf(child, toNode)) {
      discard(child, emitCreatedEvent);
      return;
    }
  }

  if (rejectsChild(toNode, toName)) {
    discard(child, emitCreatedEvent);
    return;
  }

  // A scan saw it where this takes it, or where it was: its events tell the rest.
  auto sighting = mSightings.find(childWatchDescriptor);
  if (sighting != mSightings.end() && (
    (sighting->second.parentWatchDescriptor == fromWd && sighting->second.name == fromName) ||
    (sighting->second.parentWatchDescriptor == toWd && sighting->second.name == toName)
  )) {
    eraseSighting(childWatchDescriptor);
  }

  // What it replaces may still exist (exchanged, or what a symlink there led to), so its watch is removed, not assumed
  // dropped. It's discarded after the child takes the place, so that directories a scan saw below the child can take
  // theirs.
  fromNode->pullChild(fromName);
  InotifyNode *replaced = toNode->pullChild(toName);
  toNode->takeChildAsName(child, toName);
  if (replaced != NULL) {
    discardNode(replaced, Departure::REPLACED, emitCreatedEvent);
  }
  child = findNodeByWatchDescriptor(childWatchDescriptor);
  if (child == NULL) {
    return;
  }
  applyExclusions(child, emitCreatedEvent);
  child = findNodeByWatchDescriptor(childWatchDescriptor);
  if (child != NULL) {
    settleLinksTo(child, emitCreatedEvent);
  }
  // The excluded directories it held may have taken it along, with the places they went to.
  child = findNodeByWatchDescriptor(childWatchDescriptor);
  if (child != NULL && !mIncompleteScans.empty()) {
    rescanIncomplete(child, emitCreatedEvent);
  }
}

// Whether the event loop drops an event for wd, taken back behind a marker not read yet. Events about the kernel watch
// itself are kept.
bool InotifyTree::isQuarantined(int wd, uint32_t mask) {
  return !(mask & (uint32_t)(IN_IGNORED | IN_DELETE_SELF)) && mQuarantined.count(wd) > 0;
}

// The IN_IGNORED of wd, read: all its events were. A marker lifts the quarantines up to it; a dropped watch the kernel
// removed (its directory deleted, or unmounted) is forgotten.
void InotifyTree::watchIgnored(int wd) {
  mDroppedWatches.erase(wd);
  auto removed = mRemovedWatches.find(wd);
  if (removed == mRemovedWatches.end()) {
    return;
  }
  mReadRemoval = std::max(mReadRemoval, removed->second.removal);
  uint64_t marker = removed->second.marker;
  mRemovedWatches.erase(removed);
  if (marker != 0) {
    for (auto i = mQuarantined.begin(); i != mQuarantined.end();) {
      i = i->second <= marker ? mQuarantined.erase(i) : std::next(i);
    }
    releaseSighted(marker);
  }
}

// The marker with this sequence was read, or every one was lost: the sightings behind it are watched after a read
// that leaves no move pending, see retryWatches().
void InotifyTree::releaseSighted(uint64_t marker) {
  for (auto i = mSightedBehindMarker.begin(); i != mSightedBehindMarker.end();) {
    if (i->first <= marker) {
      mRetries.push_back(i->second);
      i = mSightedBehindMarker.erase(i);
    } else {
      ++i;
    }
  }
}

// After the event loop handled a read. rm_watch queues its IN_IGNORED before it returns, so a queue read empty with
// IN_IGNORED events still expected lost them (an overflow ends the watcher anyway): what they would end, ends.
void InotifyTree::finishRead(int eventsRead, size_t bytesRead) {
  mBytesHandled += bytesRead;
  // Most reads have no maintenance to do. Check headroom when a pass actually asks to queue a marker.
  if (mRemovedWatches.empty() && mDroppedWatches.empty() && !mDeferred) {
    mMarkerCredit = mMarkerBatch;
    return;
  }
  int queued = getQueuedBytes();
  mMarkerCredit = queued == 0 ? mMarkerBatch : std::min(mMarkerBatch, std::max(1, eventsRead / 2));
  if (!mRemovedWatches.empty() && queued == 0) {
    mRemovedWatches.clear();
    for (auto i = mQuarantined.begin(); i != mQuarantined.end();) {
      i = i->second <= mMarkerSequence ? mQuarantined.erase(i) : std::next(i);
    }
    releaseSighted(mMarkerSequence);
  }
}

// A marker queued now, for the pass that ends: a dropped watch is removed, which no node of the pass holds (one that a
// later pass finds then gets a new watch), or else a node of the pass gets a new watch, its old one being the marker.
// Its sequence number, 0 if none could be queued: every node of the pass is then gone, see InotifyNode::rewatch().
uint64_t InotifyTree::queueMarker(std::vector<Found> &pass) {
  uint64_t sequence = mMarkerSequence + 1;
  while (!mDroppedWatches.empty()) {
    if (removeWatch(mDroppedWatches.begin()->first, sequence)) {
      return mMarkerSequence = sequence;
    }
  }
  for (Found &found : pass) {
    InotifyNode *node = findNodeByWatchDescriptor(found.watchDescriptor);
    if (node == NULL) {
      continue;
    }
    RestorationScope restoration(this, node->foundByExclusionUpdate());
    int replaced = 0;
    // Where a scan saw its directory meanwhile, which its own events may never tell, see sight().
    auto sighted = mSightings.find(found.watchDescriptor);
    bool hasSighting = sighted != mSightings.end();
    Sighting sighting = hasSighting ? sighted->second : Sighting();
    if (hasSighting) {
      eraseSighting(found.watchDescriptor);
      sighting.fromExclusionUpdate = sighting.fromExclusionUpdate || mRestoringExclusions;
    }
    int oldWatchDescriptor = found.watchDescriptor;
    bool kept = node->rewatch(sequence, replaced);
    // Still scanned after the marker, by its new watch descriptor.
    found.watchDescriptor = kept ? node->getWatchDescriptor() : -1;
    if (kept && found.watchDescriptor != oldWatchDescriptor) {
      // A rename into it may have moved a sighting there already, see renameDirectory().
      moveSightingsIn(oldWatchDescriptor, found.watchDescriptor);
    }
    if (hasSighting && kept) {
      setSighting(found.watchDescriptor, sighting);
    } else if (hasSighting && replaced != 0) {
      // Its node goes: its directory left this path before it was watched again, so the events that tell where it went
      // are mostly queued before this marker: once that's read, a move to a watched path has a node there, and the
      // sighted place is watched only if nothing holds it, see releaseSighted(). The kernel queues the halves of a
      // move one after the other, after the move. The marker may come between them: the event loop doesn't retry while
      // a move is pending, for its pairing wait at most. Or before both, if the move happened just before the marker,
      // which nothing here can tell.
      mSightedBehindMarker.push_back({ sequence, sighting });
    } else if (hasSighting) {
      // No marker: the kernel removed its watch already, its directory is gone.
      mRetries.push_back(sighting);
    }
    if (replaced != 0) {
      mMarkerSequence = sequence;
    }
    if (!kept) {
      // Gone or replaced: what its parent holds there is up to the events still queued, or a rescan.
      InotifyNode *parent = node->getParent();
      if (parent != NULL) {
        if (node->registrationChanged()) {
          deferChangedEntry({ parent, node->getName(), found.emitCreatedEvent, node->foundByExclusionUpdate(), found.update, mLastRemoval, 0 });
        }
        parent->removeChild(node->getName());
        markScanIncomplete(parent);
      }
    }
    if (replaced != 0) {
      return sequence;
    }
  }
  return 0;
}

void InotifyTree::deferChangedEntry(ChangedEntry entry) {
  for (auto &pending : mChangedEntries) {
    if (pending.parent == entry.parent && pending.name == entry.name) {
      if (entry.emitCreatedEvent) pending.emitCreatedEvent = entry.emitCreatedEvent;
      pending.afterRemoval = entry.afterRemoval;
      pending.afterBytes = 0;
      if (!pending.update) pending.update = entry.update;
      pending.restoring = pending.restoring || entry.restoring;
      return;
    }
  }
  mChangedEntries.push_back(std::move(entry));
}

void InotifyTree::retryChangedEntry() {
  if (mChangedEntries.empty()) return;
  // Only the reader samples this prefix, between dispatched reads. Later events cannot extend it.
  // Where FIONREAD is unavailable, a later removal marker or an empty queue also ends the prefix.
  auto &first = mChangedEntries.front();
  if (mReadRemoval <= first.afterRemoval && (!first.afterBytes || mBytesHandled < first.afterBytes)) {
    int queued = getQueuedBytes();
    if (queued != 0) {
      if (!first.afterBytes && queued != INT_MAX) first.afterBytes = mBytesHandled + queued;
      return;
    }
  }
  ChangedEntry entry = std::move(mChangedEntries.front());
  mChangedEntries.pop_front();

  RestorationScope restoration(this, entry.restoring);
  if (!entry.parent->leadsToItself()) {
    markScanIncomplete(entry.parent);
    return;
  }
  if (entry.parent->awaitsScan()) {
    deferChangedEntry(std::move(entry));
    return;
  }
  if (entry.parent->findDescendant(entry.name)) return;
  mCurrentUpdate = std::move(entry.update);
  entry.parent->createChild(entry.name, entry.emitCreatedEvent);
  mCurrentUpdate.reset();
  removeDroppedWatches(0);
}

void InotifyTree::cancelFound() {
  mChangedEntries.clear();
  mFound.clear();
  mDeferred = false;
}

void InotifyTree::resumeFound() {
  if (!mDeferred) return;
  mDeferred = false;
  settleFound();
}

// When the outermost creation returns: the directories found again with a dropped watch get a marker queued after all
// of them were found, then are scanned. What those scans find again is the next pass. A directory is then watched like
// one that moves in for the first time: none of what its old watch queued before that marker is reported, and the
// scan after it finds what's there.
void InotifyTree::settleFound() {
  if (mSettling || mDeferred) {
    return;
  }
  mSettling = true;
  while (!mFound.empty()) {
    if (hasErrored()) {
      mFound.clear();
      break;
    }
    if (mMarkerCredit == 0 || roomInQueue(getQueuedBytes()) == 0) {
#ifdef NSFW_TEST_SLOW_1
      if (mMarkerBatch == 1) {
        int signal = open((mWatchedPath + ".nsfw-test-one-marker").c_str(), O_WRONLY | O_NOFOLLOW | O_NONBLOCK);
        if (signal != -1) {
          ssize_t written = write(signal, "1", 1);
          (void)written;
          close(signal);
        }
      }
#endif
      mDeferred = true;
      break;
    }
    std::vector<Found> pass;
    pass.swap(mFound);
    pass.erase(std::remove_if(pass.begin(), pass.end(), [this](const Found &found) {
      auto node = findNodeByWatchDescriptor(found.watchDescriptor);
      return node == NULL || node->adoption != found.adoption;
    }), pass.end());
    if (pass.empty()) break;
    uint64_t marker = queueMarker(pass);
    if (marker != 0) --mMarkerCredit;
    for (const Found &found : pass) {
      auto quarantined = mQuarantined.find(found.watchDescriptor);
      if (quarantined != mQuarantined.end() && quarantined->second == PASS_PENDING) {
        quarantined->second = marker;
      }
    }
    for (const Found &found : pass) {
      InotifyNode *node = findNodeByWatchDescriptor(found.watchDescriptor);
      bool fromExclusionUpdate;
      if (node == NULL || node->adoption != found.adoption || !node->takeFoundAgain(fromExclusionUpdate)) {
        continue;
      }
      RestorationScope restoration(this, fromExclusionUpdate);
      // Its path may lead to another directory by now, which a rename still queued put there: the scan would hang that
      // one's contents below this node. Gone meanwhile, or elsewhere: a rename still queued may have taken it, and the
      // rescan after it finds what it holds, see rescanIncomplete().
      auto previous = std::move(mCurrentUpdate);
      mCurrentUpdate = found.update;
      if (!node->leadsToItself() || !node->scan(found.emitCreatedEvent)) {
        // What it holds is reported by the rescan instead, if this scan would have.
        markScanIncomplete(node, (bool)found.emitCreatedEvent);
      }
      mCurrentUpdate = std::move(previous);
    }
  }
  mSettling = false;
}

void InotifyTree::markScanIncomplete(InotifyNode *node, bool unreported) {
  IncompleteScan &mark = mIncompleteScans[node];
  mark.since = std::chrono::steady_clock::time_point();
  mark.unreported = mark.unreported || unreported;
  mark.fromExclusionUpdate = mark.fromExclusionUpdate || mRestoringExclusions;
}

// Once no event is queued, the renames that may have taken a directory from a scan have been handled, except one the
// kernel hasn't queued yet: it does right after it moves the directory. A node whose path doesn't lead to it anymore
// keeps its mark for that one, for a while.
void InotifyTree::forgetIncompleteScans() {
  if (mIncompleteScans.empty() || getQueuedBytes() != 0) {
    return;
  }
  auto now = std::chrono::steady_clock::now();
  for (auto mark = mIncompleteScans.begin(); mark != mIncompleteScans.end();) {
    std::chrono::steady_clock::time_point &since = mark->second.since;
    if (mark->first->leadsToItself() || (
      since != std::chrono::steady_clock::time_point() && now - since > STALE_SCAN_MARK_LIFETIME
    )) {
      mark = mIncompleteScans.erase(mark);
    } else {
      if (since == std::chrono::steady_clock::time_point()) {
        since = now;
      }
      ++mark;
    }
  }
}

// Scans again the nodes moved with `moved` whose scan missed a directory that went away meanwhile: the rename may be
// what took it away, if it happened while they were being scanned. Only while a node's path leads to it, since a rename
// still queued may have put another directory there; the mark stays for that rename.
void InotifyTree::rescanIncomplete(InotifyNode *moved, EmitCreatedEvent emitCreatedEvent) {
  std::vector<InotifyNode *> marked;
  for (const auto &mark : mIncompleteScans) {
    marked.push_back(mark.first);
  }
  for (InotifyNode *node : marked) {
    // A rescan may drop marked nodes, or move them out from under `moved`.
    if (mIncompleteScans.count(node) == 0) {
      continue;
    }
    InotifyNode *ancestor = node;
    while (ancestor != NULL && ancestor != moved) {
      ancestor = ancestor->getParent();
    }
    if (ancestor == NULL || !node->leadsToItself()) {
      continue;
    }

    IncompleteScan mark = mIncompleteScans[node];
    mIncompleteScans.erase(node);
    RestorationScope restoration(this, mark.fromExclusionUpdate);
    Creating creating(this);
    if (!node->scan(emitCreatedEvent, true, mark.unreported)) {
      markScanIncomplete(node, mark.unreported);
    }
  }
}

void InotifyTree::setError(const std::string &error) {
  std::lock_guard<std::mutex> lock(mErrorMutex);
  // Errors are terminal and the poll thread reports only one, so the first is kept: it's the cause, and the ones after
  // it tend to come from the same failure (say, the queue overflowing in the mkdir flood that hit the watch limit).
  // getError() still reports a watched path that is gone over it.
  if (mError.empty()) {
    mError = error;
  }
}

bool InotifyTree::addInode(ino_t inodeNumber, InotifyNode *node) {
  return inodes.insert(std::pair<ino_t, InotifyNode *>(inodeNumber,node)).second;
}

void InotifyTree::removeInode(ino_t inodeNumber) {
  inodes.erase(inodeNumber);
}

InotifyTree::InotifyNode * InotifyTree::findNodeByInode(ino_t inodeNumber) {
  auto nodeIterator = inodes.find(inodeNumber);
  if (nodeIterator == inodes.end()) {
    return NULL;
  }

  return nodeIterator->second;
}

// The node that has the directory `file` is of, at whatever path, NULL if none has it.
InotifyTree::InotifyNode * InotifyTree::findNodeOf(const struct stat &file) {
  InotifyNode *node = S_ISDIR(file.st_mode) ? findNodeByInode(file.st_ino) : NULL;
  return node != NULL && node->getDevice() == file.st_dev && node->getInodeNumber() == file.st_ino ? node : NULL;
}

InotifyTree::InotifyNode * InotifyTree::findNodeByPath(const std::string path) {
  struct stat file;
  return stat(path.c_str(), &file) == 0 ? findNodeOf(file) : NULL;
}

// The node the tree has at path, the path it got from the events handled so far, wherever its directory is now.
InotifyTree::InotifyNode *InotifyTree::findNodeAtTreePath(const std::string &path) {
  InotifyNode *root = mRoot.load();
  if (root == NULL || path == mWatchedPath) {
    return root;
  }
  std::string prefix = mWatchedPath == "/" ? mWatchedPath : mWatchedPath + "/";
  if (path.size() <= prefix.size() || path.compare(0, prefix.size(), prefix) != 0) {
    return NULL;
  }
  return root->findDescendant(path.substr(prefix.size()));
}

InotifyTree::InotifyNode *InotifyTree::findAttachedAlias(const std::string &path) {
  InotifyNode *node = findNodeByPath(path);
  if (node == NULL || findNodeAtTreePath(node->getFullPath()) != node || !node->leadsToItself()) {
    return NULL;
  }
  return node;
}

InotifyTree::~InotifyTree() {
  delete mRoot.exchange(NULL);
  delete mInotifyNodeByWatchDescriptor;
}

InotifyTree::InotifyNode *InotifyTree::findNodeByWatchDescriptor(int watchDescriptor) {
  auto nodeIterator = mInotifyNodeByWatchDescriptor->find(watchDescriptor);
  return nodeIterator == mInotifyNodeByWatchDescriptor->end() ? NULL : nodeIterator->second;
}

// A scan found, at `name` in `scanning`, a directory whose inode another node holds. Either the directory moved here
// and the events of that move aren't handled yet, or it's seen twice (through a symlink or a bind mount). Nothing is
// decided now: the place is remembered, and the directory is watched there if the holder's own events say it left for a
// place that can't be watched, see discard().
void InotifyTree::sight(InotifyNode *scanning, const std::string &name, const struct stat &file, bool throughSymlink) {
  InotifyNode *holder = findNodeOf(file);
  if (
    holder == NULL || holder->getParent() == NULL ||
    holder->getWatchDescriptor() == -1 || isAncestorOrSelf(holder, scanning) ||
    holder->getParent()->findDescendant(holder->getName()) != holder
  ) {
    return;
  }
  if (throughSymlink) {
    // A symlink never takes the place of where the directory itself was seen.
    auto seen = mSightings.find(holder->getWatchDescriptor());
    if (
      seen != mSightings.end() && !seen->second.throughSymlink && findPlace(seen->second) != NULL
    ) {
      return;
    }
    // A symlink to a directory that is still where its node is is just another way there, which a link-heavy tree
    // (pnpm) has plenty of; it's only worth remembering once the directory left.
    if (holder->leadsToItself()) {
      return;
    }
  }
  // A scanning node whose path leads elsewhere listed another directory: a rename of it is still queued, and the
  // rescan after it finds what it holds then.
  if (!scanning->leadsToItself()) {
    markScanIncomplete(scanning);
    return;
  }
  setSighting(holder->getWatchDescriptor(), {
    scanning->getWatchDescriptor(), name, file.st_dev, file.st_ino, throughSymlink, mRestoringExclusions
  });
}

// The node a scan saw at `name` in the directory with watch descriptor wd, by that name itself rather than through a
// symlink, while the tree has it elsewhere. NULL if there is none.
InotifyTree::InotifyNode *InotifyTree::findSightedAt(int wd, const std::string &name) {
  auto sightedIn = mSightedIn.find(wd);
  if (sightedIn == mSightedIn.end()) {
    return NULL;
  }
  for (int holder : sightedIn->second) {
    auto seen = mSightings.find(holder);
    if (seen == mSightings.end() || seen->second.throughSymlink || seen->second.name != name) {
      continue;
    }
    InotifyNode *node = findNodeByWatchDescriptor(holder);
    if (node != NULL) {
      return node;
    }
  }
  return NULL;
}

// The node of the directory a scan saw the sighted one in, NULL once that directory left the tree.
InotifyTree::InotifyNode *InotifyTree::findPlace(const Sighting &sighting) {
  return findNodeByWatchDescriptor(sighting.parentWatchDescriptor);
}

void InotifyTree::setSighting(int holder, const Sighting &sighting) {
  eraseSighting(holder);
  mSightings[holder] = sighting;
  mSightedIn[sighting.parentWatchDescriptor].insert(holder);
}

// The directory a scan saw the holder's directory in had an entry renamed to it: that place is now toName in toWd.
void InotifyTree::moveSighting(int holder, int toWd, const std::string &toName) {
  Sighting sighting = mSightings.at(holder);
  sighting.parentWatchDescriptor = toWd;
  sighting.name = toName;
  setSighting(holder, sighting);
}

// The directory with watch descriptor fromWd, where scans saw directories, has toWd now: also for the ones still to
// be watched there, see mRetries and mSightedBehindMarker.
void InotifyTree::moveSightingsIn(int fromWd, int toWd) {
  auto sightedIn = mSightedIn.find(fromWd);
  if (sightedIn != mSightedIn.end()) {
    std::set<int> holders = sightedIn->second;
    for (int holder : holders) {
      moveSighting(holder, toWd, mSightings.at(holder).name);
    }
  }
  auto repoint = [fromWd, toWd](Sighting &sighting) {
    if (sighting.parentWatchDescriptor == fromWd) {
      sighting.parentWatchDescriptor = toWd;
    }
  };
  for (Sighting &retry : mRetries) {
    repoint(retry);
  }
  for (auto &behind : mSightedBehindMarker) {
    repoint(behind.second);
  }
}

void InotifyTree::eraseSighting(int holder) {
  auto seen = mSightings.find(holder);
  if (seen == mSightings.end()) {
    return;
  }
  auto sightedIn = mSightedIn.find(seen->second.parentWatchDescriptor);
  sightedIn->second.erase(holder);
  if (sightedIn->second.empty()) {
    mSightedIn.erase(sightedIn);
  }
  mSightings.erase(seen);
}

// holder, a node at a symlink, can't stay there: the symlink doesn't lead to its directory anymore, or goes with
// `leaving`. Where a scan saw the directory itself, and it still is, the node takes that place with what is below it,
// unless the place goes with `leaving` or an excluded path names it. No watch is dropped, so no event is: from then on
// they are reported at the directory's own path. Unlike rebornSighted(), which is for a directory that moved: this one
// never left that place, so its watch and what it queued stay good. The caller settles it there, see
// settleAtOwnPlace().
InotifyTree::OwnPlace InotifyTree::takeOwnPlace(InotifyNode *holder, InotifyNode *leaving) {
  int watchDescriptor = holder->getWatchDescriptor();
  auto seen = mSightings.find(watchDescriptor);
  // The root, not any node without a parent: one a rename replaced is out of the tree when it's discarded.
  if (!holder->isThroughSymlink() || holder == mRoot.load() || seen == mSightings.end() ||
    seen->second.throughSymlink) {
    return OwnPlace::NONE;
  }
  const std::string name = seen->second.name;
  InotifyNode *place = findPlace(seen->second);
  if (
    place == NULL || isAncestorOrSelf(holder, place) || isAncestorOrSelf(leaving, place) ||
    !isAncestorOrSelf(mRoot.load(), place) || !place->leadsToItself() ||
    !leadsTo(
      InotifyNode::createFullPath(place->getFullPath(), name), false, holder->getDevice(), holder->getInodeNumber()
    )
  ) {
    return OwnPlace::NONE;
  }
  if (rejectsChild(place, name)) {
    return OwnPlace::EXCLUDED;
  }
  // A node left there is of a directory that is gone, whose events are still queued. Not one holder is below: those
  // events are then about holder too.
  InotifyNode *replaced = place->findDescendant(name);
  if (replaced != NULL && isAncestorOrSelf(replaced, holder)) {
    return OwnPlace::NONE;
  }
  place->removeChild(name);
  eraseSighting(watchDescriptor);
  if (holder->getParent() != NULL) {
    holder->getParent()->pullChild(holder->getName());
  }
  place->takeChildAsName(holder, name);
  holder->setThroughSymlink(false);
  mWaysChanged = true;
  return OwnPlace::TAKEN;
}

// node took its own place: the directories below it that an excluded path names by that path go. The names rejected
// by the symlink's path stay so until an update or a rename asks about them: they were excluded all along, and a
// symlink removed is often made again right away. What a scan below it missed while its path led nowhere is scanned
// again, see rescanIncomplete(). And the other symlinks that led below it by its old path may lead nowhere now, see
// settleLinksTo().
void InotifyTree::settleAtOwnPlace(InotifyNode *node, EmitCreatedEvent emitCreatedEvent) {
  int watchDescriptor = node->getWatchDescriptor();
  excludeNamed(node, mExcludedNames, emitCreatedEvent);
  node = findNodeByWatchDescriptor(watchDescriptor);
  if (node != NULL && !mIncompleteScans.empty()) {
    rescanIncomplete(node, emitCreatedEvent);
  }
  node = findNodeByWatchDescriptor(watchDescriptor);
  if (node != NULL) {
    settleLinksTo(node, emitCreatedEvent);
  }
}

InotifyTree::OwnPlace InotifyTree::moveToOwnPlace(InotifyNode *holder, EmitCreatedEvent emitCreatedEvent) {
  OwnPlace place = takeOwnPlace(holder, NULL);
  if (place == OwnPlace::TAKEN) {
    settleAtOwnPlace(holder, emitCreatedEvent);
  }
  return place;
}

// The symlink holder is at may not lead to its directory anymore, after a rename of what it led through. Nothing can
// be watched below a path that leads nowhere, so the node takes the directory's own place, or goes if the rename took
// the directory to an excluded one. With no such place known it stays, and its events keep the symlink's path.
void InotifyTree::settleLink(InotifyNode *holder, EmitCreatedEvent emitCreatedEvent) {
  if (!holder->isAtStaleLink()) {
    return;
  }
  if (moveToOwnPlace(holder, emitCreatedEvent) == OwnPlace::EXCLUDED) {
    discard(holder, emitCreatedEvent);
  }
}

// settleLink() for the node with watchDescriptor, if it's still in the tree.
void InotifyTree::settleLinkOf(int watchDescriptor, EmitCreatedEvent emitCreatedEvent) {
  InotifyNode *holder = findNodeByWatchDescriptor(watchDescriptor);
  if (holder != NULL) {
    settleLink(holder, emitCreatedEvent);
  }
}

// moved has another path now: the symlinks that moved with it, and the ones that led to it or below it by its old
// path, may lead nowhere, see settleLink(). Only the nodes it has are looked at, not every sighting: a link-heavy tree
// has thousands.
void InotifyTree::settleLinksTo(InotifyNode *moved, EmitCreatedEvent emitCreatedEvent) {
  SightedBelow sighted = findSightedBelow(moved);
  for (int watchDescriptor : sighted.held) {
    settleLinkOf(watchDescriptor, emitCreatedEvent);
  }
  for (int watchDescriptor : sighted.seenIn) {
    settleLinkOf(watchDescriptor, emitCreatedEvent);
  }
}

// An entry changed at `name` in the directory with wd: the ways that looked it up may lead elsewhere. And if the tree
// has a node there at a symlink that was removed or repointed, the node takes its directory's own place. With none to
// take it stays, also where that place is excluded: a symlink made again finds its directory watched.
void InotifyTree::entryChanged(int wd, const std::string &name, EmitCreatedEvent emitCreatedEvent) {
  InotifyNode *parent = findNodeByWatchDescriptor(wd);
  if (parent == NULL) {
    return;
  }
  lookupChanged(parent, name);
  InotifyNode *child = parent->findDescendant(name);
  if (child != NULL && child->isAtStaleLink()) {
    moveToOwnPlace(child, emitCreatedEvent);
  }
}

// An event about `name` in the directory with wd was dropped, see isQuarantined(): what it told a way is not.
void InotifyTree::entryDropped(int wd, const std::string &name) {
  InotifyNode *directory = findNodeByWatchDescriptor(wd);
  if (directory != NULL) {
    lookupChanged(directory, name);
  }
}

// The nodes held at node or below it that a scan saw elsewhere, and the ones elsewhere that a scan saw at node or below
// it, by watch descriptors. Looked for in its subtree rather than among every sighting, which a bind mount of a large
// tree makes as many as its directories, and a link-heavy tree (pnpm) thousands.
InotifyTree::SightedBelow InotifyTree::findSightedBelow(InotifyNode *node) {
  SightedBelow sighted;
  if (mSightings.empty()) {
    return sighted;
  }
  auto collect = [this, &sighted](InotifyNode *below) {
    int watchDescriptor = below->getWatchDescriptor();
    if (mSightings.count(watchDescriptor) > 0) {
      sighted.held.push_back(watchDescriptor);
    }
    auto sightedIn = mSightedIn.find(watchDescriptor);
    if (sightedIn != mSightedIn.end()) {
      sighted.seenIn.insert(sighted.seenIn.end(), sightedIn->second.begin(), sightedIn->second.end());
    }
  };
  collect(node);
  node->forEachBelow(collect);
  return sighted;
}

// For node, which goes with what is below it: the directories held below it that a scan saw elsewhere take their own
// place, or else are watched where they were seen once node is deleted, see settleKept(). The ones held at symlinks
// elsewhere that a scan saw in it are handed to settleStranded(), which settles them at their own place or discards
// them. Not for a node a rename replaced: its directory may still be in the watched path, exchanged with the one that
// replaced it.
void InotifyTree::keepSightedBelow(InotifyNode *node, Departure departure, DiscardSteps &steps) {
  SightedBelow sighted = findSightedBelow(node);
  if (departure == Departure::LEFT) {
    for (int watchDescriptor : sighted.seenIn) {
      InotifyNode *holder = findNodeByWatchDescriptor(watchDescriptor);
      if (holder != NULL && !isAncestorOrSelf(node, holder) && !mSightings.at(watchDescriptor).throughSymlink) {
        steps.stranded.push_back(watchDescriptor);
      }
    }
  }
  for (int watchDescriptor : sighted.held) {
    InotifyNode *holder = findNodeByWatchDescriptor(watchDescriptor);
    // Not node itself (see rebornSighted()), nor one no longer below it (it left with a holder above it).
    if (holder == NULL || holder == node || !isAncestorOrSelf(node, holder)) {
      continue;
    }
    if (takeOwnPlace(holder, node) == OwnPlace::TAKEN) {
      steps.rehomed.push_back(watchDescriptor);
    } else {
      rebornSighted(watchDescriptor, node, steps);
    }
  }
}

void InotifyTree::settleKept(const DiscardSteps &steps, EmitCreatedEvent emitCreatedEvent) {
  for (int watchDescriptor : steps.rehomed) {
    InotifyNode *rehomed = findNodeByWatchDescriptor(watchDescriptor);
    if (rehomed != NULL) {
      settleAtOwnPlace(rehomed, emitCreatedEvent);
    }
  }
  {
    // One pass for all of them, so one marker: there may be as many sightings as directories.
    Creating creating(this);
    for (const Sighting &sighting : steps.reborn) {
      watchSighted(sighting, emitCreatedEvent);
    }
  }
  settleStranded(steps.stranded, emitCreatedEvent);
}

// The nodes at symlinks whose directories a scan saw in a subtree that left, and whose symlink leads nowhere now: if
// that place left with it, so did their directories. If it stayed in the tree (it took its own place, or is watched
// again where a scan saw it too, see rebornSighted()), they take theirs there, see settleLink().
void InotifyTree::settleStranded(const std::vector<int> &stranded, EmitCreatedEvent emitCreatedEvent) {
  for (int watchDescriptor : stranded) {
    InotifyNode *holder = findNodeByWatchDescriptor(watchDescriptor);
    if (holder == NULL || !holder->isAtStaleLink()) {
      continue;
    }
    auto seen = mSightings.find(watchDescriptor);
    if (seen != mSightings.end() && findPlace(seen->second) == NULL) {
      discard(holder, emitCreatedEvent);
    } else {
      settleLink(holder, emitCreatedEvent);
    }
  }
}

// For the holder with watchDescriptor, which leaves: if a scan saw its directory at a place that still holds it, that
// sighting goes to steps.reborn, to be watched there once the holder is deleted, like a directory found there for the
// first time (watchSighted() takes its dropped watch back behind a marker, see InotifyNode::inotifyInit()). The
// holder's node never moves there: its watch's events since it left may be from where it couldn't be watched.
void InotifyTree::rebornSighted(int watchDescriptor, InotifyNode *leaving, DiscardSteps &steps) {
  auto found = mSightings.find(watchDescriptor);
  if (found == mSightings.end()) {
    return;
  }
  Sighting sighting = found->second;
  eraseSighting(watchDescriptor);
  RestorationScope restoration(this, sighting.fromExclusionUpdate);

  InotifyNode *holder = findNodeByWatchDescriptor(watchDescriptor);
  InotifyNode *place = findPlace(sighting);
  if (
    holder == NULL || place == NULL || holder == mRoot.load() ||
    (place == holder->getParent() && sighting.name == holder->getName()) ||
    // Not into itself, nor into what leaves with it, nor into a subtree already dropped.
    isAncestorOrSelf(holder, place) || isAncestorOrSelf(leaving, place) ||
    !isAncestorOrSelf(mRoot.load(), place)
  ) {
    return;
  }
  if (rejectsChild(place, sighting.name)) {
    return;
  }
  // Only to a place checked to hold the directory. One whose path leads elsewhere has a rename still queued, whose
  // rescan finds the directory there if it still is.
  if (!place->leadsToItself()) {
    markScanIncomplete(place);
    return;
  }
  // Still there (or one that got its inode number since: watched there all the same, once the holder no longer claims
  // the number).
  std::string path = InotifyNode::createFullPath(place->getFullPath(), sighting.name);
  if (leadsTo(path, sighting.throughSymlink, sighting.device, sighting.inodeNumber)) {
    steps.reborn.push_back(sighting);
  }
}

// Watches a sighted directory after its previous holder has been discarded or a watch retry becomes possible.
void InotifyTree::watchSighted(const Sighting &sighting, EmitCreatedEvent emitCreatedEvent) {
  RestorationScope restoration(this, sighting.fromExclusionUpdate);
  InotifyNode *place = findPlace(sighting);
  if (place == NULL) {
    return;
  }
  if (!place->leadsToItself()) {
    // A rename of it is still queued, and the rescan after it finds the directory, see rescanIncomplete().
    markScanIncomplete(place);
    return;
  }
  std::string path = InotifyNode::createFullPath(place->getFullPath(), sighting.name);
  if (
    leadsTo(path, sighting.throughSymlink, sighting.device, sighting.inodeNumber) &&
    place->createChild(sighting.name, emitCreatedEvent)
  ) {
    markScanIncomplete(place);
  }
}

// node left, as far as its events tell, for a place that can't be watched: outside the watched path, or into an
// excluded directory, or an excluded path names it now. Unless it's no longer the tree's node of its directory, or is
// the root.
void InotifyTree::discard(InotifyNode *node, EmitCreatedEvent emitCreatedEvent) {
  if (findNodeByWatchDescriptor(node->getWatchDescriptor()) != node || node->getParent() == NULL) {
    return;
  }
  discardNode(node, Departure::LEFT, emitCreatedEvent);
}

// node goes, with its subtree: one that left, see discard(), or one a rename replaced, already out of the tree, see
// renameDirectory(). Where a scan saw its directory, or one of the directories below it, at a place that still holds
// it, that directory stays: at a symlink until now, its node takes its own place, see takeOwnPlace(); moved there, it
// is watched like one found there for the first time, see rebornSighted().
void InotifyTree::discardNode(InotifyNode *node, Departure departure, EmitCreatedEvent emitCreatedEvent) {
  if (moveToOwnPlace(node, emitCreatedEvent) == OwnPlace::TAKEN) {
    return;
  }
  DiscardSteps steps;
  keepSightedBelow(node, departure, steps);
  rebornSighted(node->getWatchDescriptor(), node, steps);
  if (node->getParent() != NULL) {
    node->getParent()->pullChild(node->getName());
  }
  delete node;
  settleKept(steps, emitCreatedEvent);
}

// The watched path itself, when excluded, is still watched: its events are dropped instead, see isRootExcluded().
bool InotifyTree::isExcluded(const std::string &path) const {
  return path != mWatchedPath && mExcluded.count(path) > 0;
}

// Where the names below ancestor start in path, the end of path if it is ancestor itself, 0 if neither.
static size_t findNamesBelow(const std::string &path, const std::string &ancestor) {
  const size_t length = ancestor == "/" ? 0 : ancestor.size();
  if (ancestor.empty() || path.compare(0, length, ancestor, 0, length) != 0) {
    return 0;
  }
  return path.size() == length ? length : (path[length] == '/' ? length + 1 : 0);
}

// Where the names below the watched path start in path, 0 if it isn't below it.
size_t InotifyTree::findNamesBelowWatchedPath(const std::string &path) const {
  size_t names = findNamesBelow(path, mWatchedPath);
  return names < path.size() ? names : 0;
}

// The names of path from start on, last first, onto names.
static void pushNames(std::vector<std::string> &names, const std::string &path, size_t start) {
  for (size_t end = path.size(); end > start;) {
    size_t slash = path.find_last_of('/', end - 1);
    size_t name = slash == std::string::npos || slash < start ? start : slash + 1;
    if (name < end) {
      names.push_back(path.substr(name, end - name));
    }
    end = name == start ? start : name - 1;
  }
}

void InotifyTree::setExcludedPaths(const std::vector<std::string> &excludedPaths) {
  mExcludedPaths = excludedPaths;
  mExcluded = std::unordered_set<std::string>(excludedPaths.begin(), excludedPaths.end());
  mExcludedNames.clear();
  InotifyNode *root = mRoot.load();
  std::unordered_set<std::string> ways;
  for (const std::string &path : mExcludedPaths) {
    mExcludedNames.insert(path.substr(path.find_last_of('/') + 1));
    // Every directory it goes through, from the last one up to the watched path, or to one another path gave.
    const size_t names = findNamesBelowWatchedPath(path);
    for (size_t end = path.find_last_of('/'); names != 0; end = path.find_last_of('/', end - 1)) {
      const std::string way = end == 0 ? "/" : path.substr(0, end);
      if (!ways.insert(way).second) {
        break;
      }
      // The ways an update keeps are not followed again.
      auto added = mWays.insert({ way, { way, false, Directory(), {} } });
      if (added.second) {
        followWay(added.first->second, root);
      }
      if (end < names) {
        break;
      }
    }
  }
  for (auto way = mWays.begin(); way != mWays.end();) {
    if (ways.count(way->first) > 0) {
      ++way;
    } else {
      indexWay(way->second, false);
      way = mWays.erase(way);
    }
  }
}

// Puts a way in the indexes, or takes it out of them.
void InotifyTree::indexWay(Way &way, bool indexed) {
  auto change = [&way, indexed](auto &ways, const auto &key) {
    if (indexed) {
      ways[key].insert(&way);
      return;
    }
    auto found = ways.find(key);
    if (found != ways.end() && found->second.erase(&way) > 0 && found->second.empty()) {
      ways.erase(found);
    }
  };
  if (way.leads) {
    change(mWaysTo, way.directory);
  }
  for (const auto &lookup : way.lookups) {
    change(mWaysByLookup, lookup);
  }
}

// Follows a way on disk, name by name from the watched path. It leads to a directory if it never leaves the watched
// path, nor looks a name up in a directory nothing watches: one the tree rejected and has at no other path. One with
// no node yet is about to be scanned. So whatever makes it lead elsewhere changes one of its lookups, an entry of a
// watched directory or a directory that gets watched, rejected or dropped, and it is followed again then, see
// lookupChanged() and directoryChanged(). An absolute symlink stays in the watched path if it spells it as this
// watcher has it, which leads to it for as long as the watcher works.
void InotifyTree::followWay(Way &way, InotifyNode *root) {
  struct Followed {
    std::string path;
    Directory directory;
    InotifyNode *node;
    bool watched;
  };
  indexWay(way, false);
  const bool led = way.leads;
  const Directory was = way.directory;
  way.leads = true;
  way.lookups.clear();
  // The directories followed so far, the watched path first, and the names still to follow, last first.
  std::vector<Followed> followed = { { mWatchedPath, mWatchedDirectory, root, true } };
  std::vector<std::string> names;
  pushNames(names, way.path, findNamesBelow(way.path, mWatchedPath));
  for (int links = 0; way.leads && !names.empty();) {
    const std::string name = names.back();
    names.pop_back();
    if (name == ".") {
      continue;
    }
    if (name == "..") {
      followed.pop_back();
      way.leads = !followed.empty();
      continue;
    }
    const Followed in = followed.back();
    const std::string next = InotifyNode::createFullPath(in.path, name);
    way.lookups.push_back({ in.directory, name });
    struct stat file;
    if (!in.watched || lstat(next.c_str(), &file) != 0) {
      way.leads = false;
    } else if (S_ISLNK(file.st_mode)) {
      char target[PATH_MAX];
      ssize_t length = readlink(next.c_str(), target, sizeof(target));
      const std::string linked(target, length < 0 ? 0 : length);
      const size_t start = linked[0] == '/' ? findNamesBelow(linked, mWatchedPath) : 0;
      way.leads = length > 0 && length < (ssize_t)sizeof(target) && ++links <= MAX_LINKS_FOLLOWED &&
        (linked[0] != '/' || start != 0);
      if (way.leads && linked[0] == '/') {
        followed.erase(followed.begin() + 1, followed.end());
      }
      if (way.leads) {
        pushNames(names, linked, start);
      }
    } else if (S_ISDIR(file.st_mode)) {
      InotifyNode *node = findNodeOf(file);
      bool watched = node != NULL || in.node == NULL || !isRejected(in.node, name);
      followed.push_back({ next, { file.st_dev, file.st_ino }, node, watched });
    } else {
      way.leads = false;
    }
  }
  if (way.leads) {
    way.directory = followed.back().directory;
  }
  indexWay(way, true);
  mWaysChanged = mWaysChanged || led != way.leads || (led && was != way.directory);
}

// The root, also while it is being built: reached from node then, which may be NULL.
InotifyTree::InotifyNode *InotifyTree::findRoot(InotifyNode *node) {
  InotifyNode *root = mRoot.load();
  while (root == NULL && node != NULL && node->getParent() != NULL) {
    node = node->getParent();
  }
  return root != NULL ? root : node;
}

// The entry `name` of a node's directory may be another now, or its directory rejected when it wasn't, or the other
// way around: the ways that looked it up are followed again.
void InotifyTree::lookupChanged(InotifyNode *directory, const std::string &name) {
  auto ways = mWaysByLookup.empty() ? mWaysByLookup.end() :
    mWaysByLookup.find({ { directory->getDevice(), directory->getInodeNumber() }, name });
  if (ways == mWaysByLookup.end()) {
    return;
  }
  InotifyNode *root = findRoot(directory);
  for (Way *way : std::vector<Way *>(ways->second.begin(), ways->second.end())) {
    followWay(*way, root);
  }
}

// A node has that directory from now on, or none does anymore: no event told what happened in it until now, or none
// will from now on, so every way that looked a name up in it is followed again. Not without a root, when the tree goes.
void InotifyTree::directoryChanged(const Directory &directory, InotifyNode *node) {
  InotifyNode *root = findRoot(node);
  std::set<Way *> ways;
  for (
    auto lookup = mWaysByLookup.lower_bound({ directory, std::string() });
    root != NULL && lookup != mWaysByLookup.end() && lookup->first.first == directory;
    ++lookup
  ) {
    ways.insert(lookup->second.begin(), lookup->second.end());
  }
  for (Way *way : ways) {
    followWay(*way, root);
  }
}

// Whether an excluded path names `name` in parent's directory by another path than the tree's: two paths can lead to
// one directory, and the tree has it at one only. One of the ways leads to the directory of parent, or of a node above
// it, and the excluded path goes on from there by the names the tree has down to `name`: on disk those may not exist
// yet, or name another directory by now. The way is looked up once more, in case it wasn't followed again in time,
// and the node's own path has to lead to its directory too, since one that is gone may have left its inode number to
// what is on disk.
bool InotifyTree::namesThroughAlias(InotifyNode *parent, const std::string &name) {
  std::string names = name;
  for (InotifyNode *node = parent; node != NULL; node = node->getParent()) {
    auto ways = mWaysTo.find({ node->getDevice(), node->getInodeNumber() });
    if (ways != mWaysTo.end()) {
      for (const Way *way : ways->second) {
        if (
          mExcluded.count(InotifyNode::createFullPath(way->path, names)) > 0 &&
          leadsTo(way->path, true, node->getDevice(), node->getInodeNumber()) && node->leadsToItself()
        ) {
          return true;
        }
      }
    }
    names = node->getName() + "/" + names;
  }
  return false;
}

// Whether an excluded path names `name` in parent's directory, which is recorded, see setRejected(). By the tree's own
// path first, which takes no lookup on disk.
bool InotifyTree::rejectsChild(InotifyNode *parent, const std::string &name) {
  Rejected rejected = Rejected::NO;
  if (mExcludedNames.count(name) > 0) {
    if (isExcluded(InotifyNode::createFullPath(parent->getFullPath(), name))) {
      rejected = Rejected::BY_PATH;
    } else if (namesThroughAlias(parent, name)) {
      rejected = Rejected::BY_ALIAS;
    }
  }
  setRejected(parent, name, rejected);
  return rejected != Rejected::NO;
}

bool InotifyTree::isRejected(InotifyNode *parent, const std::string &name) const {
  auto byPath = mRejectedByPath.find(parent);
  auto byAlias = mRejectedByAlias.find(parent);
  return (byPath != mRejectedByPath.end() && byPath->second.count(name) > 0) ||
    (byAlias != mRejectedByAlias.end() && byAlias->second.count(name) > 0);
}

template <typename Rejected>
static void eraseName(Rejected &rejected, typename Rejected::key_type parent, const std::string &name) {
  auto names = rejected.find(parent);
  if (names != rejected.end() && names->second.erase(name) > 0 && names->second.empty()) {
    rejected.erase(names);
  }
}

// Records whether an excluded path names `name` in parent's directory: the name is in mRejectedByPath or
// mRejectedByAlias exactly while the last answer was yes. Once the answer is another, so may be where the ways through
// that directory lead.
void InotifyTree::setRejected(InotifyNode *parent, const std::string &name, Rejected rejected) {
  const bool was = isRejected(parent, name);
  if (rejected != Rejected::BY_PATH) {
    eraseName(mRejectedByPath, parent, name);
  }
  if (rejected != Rejected::BY_ALIAS) {
    eraseName(mRejectedByAlias, parent, name);
  }
  if (rejected == Rejected::BY_PATH) {
    mRejectedByPath[parent].insert(name);
  } else if (rejected == Rejected::BY_ALIAS) {
    mRejectedByAlias[parent].insert(name);
  }
  if (was != (rejected != Rejected::NO)) {
    lookupChanged(parent, name);
  }
}

// Whether ancestor is node or one of its ancestors.
bool InotifyTree::isAncestorOrSelf(InotifyNode *ancestor, InotifyNode *node) {
  for (; node != NULL; node = node->getParent()) {
    if (node == ancestor) {
      return true;
    }
  }
  return false;
}

// The tree only holds what was excluded when each directory was added, by the names and the directories on disk then.
// Once those changed for node (it moved, or a directory on another path to it did), the names rejected at or below it
// that no excluded path names anymore are watched, and the directories below it that one names now are dropped.
void InotifyTree::applyExclusions(InotifyNode *node, EmitCreatedEvent emitCreatedEvent) {
  int watchDescriptor = node->getWatchDescriptor();
  {
    // The directories it includes again share a pass, so one marker, whatever their number: a marker each would remove
    // another one's dropped watch each time, while this one event is handled. The discards below see no node left
    // unscanned.
    Creating creating(this);
    includeRejected(node, false, emitCreatedEvent);
  }
  node = findNodeByWatchDescriptor(watchDescriptor);
  if (node != NULL) {
    excludeNamed(node, mExcludedNames, emitCreatedEvent);
  }
}

// Asks again about the names rejected at or below node, only those rejected by another path than the tree's with
// throughAliasOnly, and watches the ones no excluded path names anymore. With emitCreatedEvent, what they hold is
// reported as created: none of it was visible before, and a watch dropped while excluded has its events dropped too.
void InotifyTree::includeRejected(InotifyNode *node, bool throughAliasOnly, EmitCreatedEvent emitCreatedEvent) {
  std::vector<std::pair<InotifyNode *, std::string>> rejected;
  for (const auto *byParent : { &mRejectedByAlias, &mRejectedByPath }) {
    for (const auto &names : *byParent) {
      if ((!throughAliasOnly || byParent == &mRejectedByAlias) && isAncestorOrSelf(node, names.first)) {
        for (const std::string &name : names.second) {
          rejected.push_back({ names.first, name });
        }
      }
    }
  }
  for (const auto &name : rejected) {
    // A node that went meanwhile took its names along.
    InotifyNode *parent = name.first;
    if (
      !isRejected(parent, name.second) || rejectsChild(parent, name.second) ||
      parent->findDescendant(name.second) != NULL
    ) {
      continue;
    }
    // A parent whose path leads elsewhere, before or once the name is looked up, has a rename still queued: the rescan
    // after it finds the name there, see rescanIncomplete(). A name that went away by itself has events of its own.
    if (
      !parent->leadsToItself() ||
      (parent->createChild(name.second, emitCreatedEvent) && !parent->leadsToItself())
    ) {
      markScanIncomplete(parent);
    }
  }
}

// Drops the directories below node that an excluded path names, and tells whether there was one. Only one named like
// the end of an excluded path can be, so only those with one of names are looked up.
bool InotifyTree::excludeNamed(InotifyNode *node, const ExcludedNames &names, EmitCreatedEvent emitCreatedEvent) {
  std::vector<int> named;
  if (!names.empty()) {
    node->forEachBelow([&names, &named](InotifyNode *below) {
      if (names.count(below->getName()) > 0) {
        named.push_back(below->getWatchDescriptor());
      }
    });
  }
  bool dropped = false;
  for (int watchDescriptor : named) {
    // Unless an earlier one took it where it isn't excluded.
    InotifyNode *excluded = findNodeByWatchDescriptor(watchDescriptor);
    if (excluded != NULL && excluded->getParent() != NULL &&
      rejectsChild(excluded->getParent(), excluded->getName())) {
      discard(excluded, emitCreatedEvent);
      dropped = true;
    }
  }
  return dropped;
}

// Once a way leads elsewhere: an excluded path that named a directory by another path than the tree's may not
// anymore, wherever the change was (a symlink on its way removed or repointed, a directory on its way renamed or
// excluded), so those names are all asked about again. The others only change with the tree's own paths.
void InotifyTree::revalidateRejected(EmitCreatedEvent emitCreatedEvent) {
  InotifyNode *root = mRoot.load();
  if (!mWaysChanged || root == NULL) {
    return;
  }
  mWaysChanged = false;
  if (mRejectedByAlias.empty()) {
    return;
  }
  Creating creating(this);
  includeRejected(root, true, emitCreatedEvent);
}

std::string InotifyTree::getParentPath(const std::string &filePath) {
  std::string directory;
  uint32_t location = filePath.find_last_of("/");
  directory = location == 0 ? "/" : filePath.substr(0, location);
  return directory;
}

// An unscanned ancestor can still be responsible for installing this watch. It can also be
// attached through a symlink, so look for its inode as well as the spelling held in the tree.
void InotifyTree::collectPendingAncestors(std::string path, std::unordered_set<uint64_t> &ancestors) {
  while (path != mWatchedPath) {
    InotifyNode *node = findNodeAtTreePath(path);
    if (!node || !node->awaitsScan()) node = findNodeByPath(path);
    if (node && node->awaitsScan()) {
      ancestors.insert(node->adoption);
      return;
    }
    if (path == "/") break;
    auto parentPath = getParentPath(path);
    if (!mChangedEntries.empty()) {
      auto parent = findNodeAtTreePath(parentPath);
      auto alias = findAttachedAlias(parentPath);
      auto name = path.substr(path.rfind('/') + 1);
      for (ChangedEntry &entry : mChangedEntries) {
        if ((entry.parent == parent || entry.parent == alias) && entry.name == name) {
          entry.update = mCurrentUpdate;
          mCurrentUpdate->applied = false;
        }
      }
    }
    path = parentPath;
  }
}

std::future<bool> InotifyTree::updateExcludedPaths(
  const std::vector<std::string> &excludedPaths,
  EmitCreatedEvent emitCreatedEvent
) {
  mCurrentUpdate = std::make_shared<UpdateCompletion>();
  auto completion = mCurrentUpdate->done.get_future();
  std::vector<std::string> removedExcludedPaths;
  std::unordered_set<std::string> newExcludedPaths(excludedPaths.begin(), excludedPaths.end());

  for (auto excludedPath : mExcludedPaths) {
    if (newExcludedPaths.find(excludedPath) == newExcludedPaths.end()) {
      removedExcludedPaths.push_back(excludedPath);
    }
  }

  // Keep the old exclusions while a previous move scans its contents. Its visible entries still
  // need CREATED, while an explicit re-inclusion must install its own subtree silently.
  if ((!mFound.empty() || !mChangedEntries.empty()) && !removedExcludedPaths.empty()) {
    if (mRootExcluded && !newExcludedPaths.count(mWatchedPath)) {
      for (Found &found : mFound) found.update = mCurrentUpdate;
      for (ChangedEntry &entry : mChangedEntries) entry.update = mCurrentUpdate;
      mCurrentUpdate->applied = false;
    } else {
      std::unordered_set<uint64_t> ancestors;
      for (const std::string &path : removedExcludedPaths) collectPendingAncestors(path, ancestors);
      for (Found &found : mFound) {
        if (ancestors.count(found.adoption)) {
          found.update = mCurrentUpdate;
          mCurrentUpdate->applied = false;
        }
      }
    }
    if (!mCurrentUpdate->applied) {
      mCurrentUpdate.reset();
      return completion;
    }
  }

  setExcludedPaths(excludedPaths);
  mRootExcluded = std::find(mExcludedPaths.begin(), mExcludedPaths.end(), mWatchedPath) != mExcludedPaths.end();

  InotifyNode *root = mRoot.load();
  if (root != NULL) {
    {
      // The directories found again share a pass, so one marker, whatever the number of paths: a marker per path would
      // remove another path's dropped watch each time, under the lock that keeps the event loop from reading them.
      Creating creating(this);
      RestorationScope restoration(this, true);
      includeRejected(root, false);
    }
    // Every excluded path is asked about, not only the new ones: one that stays may lead where it didn't, now that
    // the paths that are gone are watched again.
    mExcluding = true;
    bool dropped = excludeNamed(root, mExcludedNames, emitCreatedEvent);
    mExcluding = false;
    if (dropped) {
      // What the directories just dropped no longer let an excluded path name, like the paths included again above.
      Creating creating(this);
      RestorationScope restoration(this, true);
      includeRejected(root, false);
    }
  }
  mCurrentUpdate.reset();
  return completion;
}

#ifdef NSFW_TEST_SLOW_1
static bool signalRegistration(const std::string &path) {
  int signal = open(path.c_str(), O_WRONLY | O_APPEND | O_NOFOLLOW | O_NONBLOCK);
  if (signal == -1) return false;
  ssize_t written = write(signal, "1", 1);
  close(signal);
  return written == 1;
}
#endif

/**
 * InotifyNode ---------------------------------------------------------------------------------------------------------
 */
InotifyTree::InotifyNode::InotifyNode(
  InotifyTree *tree,
  int inotifyInstance,
  InotifyNode *parent,
  const std::string &directory,
  std::string name,
  dev_t device,
  ino_t inodeNumber,
  bool throughSymlink,
  EmitCreatedEvent emitCreatedEvent
):
  mThroughSymlink(throughSymlink),
  mDirectory(directory),
  mDevice(device),
  mInodeNumber(inodeNumber),
  mInotifyInstance(inotifyInstance),
  mName(name),
  mParent(parent),
  mTree(tree) {
  mChildren = new std::map<std::string, InotifyNode *>;
  mFullPath = createFullPath(mDirectory, mName);
  mWatchDescriptorInitialized = false;

  #ifdef NSFW_TEST_SLOW_1
    if (mName == "nsfw-test-changing-link") signalRegistration(mTree->mWatchedPath + ".acquiring");
    std::this_thread::sleep_for(std::chrono::milliseconds(500));
  #endif

  if (!inotifyInit()) {
    return;
  }

  if (mFoundAgain) {
    mFoundByExclusionUpdate = mTree->mRestoringExclusions;
    adoption = ++mTree->mLastAdoption;
    mTree->mFound.push_back({ adoption, mTree->mCurrentUpdate, mWatchDescriptor, emitCreatedEvent });
    mAlive = true;
    return;
  }
  mAlive = scan(emitCreatedEvent);
}

// Adds a child for each directory in this one, false if it can't be read. A rescan only adds the ones it has no child
// for: replacing the others is up to the events still queued for them. It reports what the directories it adds hold
// as created, since a creation still queued for one of them finds it watched already, but not the directories
// themselves, which the scan it completes or their own events report. That can report again what a directory that
// moved here holds, which beats losing what was created in it. A rescan reports its own entries too with reportEntries,
// for a directory found again whose first scan didn't run, see settleFound().
bool InotifyTree::InotifyNode::scan(EmitCreatedEvent emitCreatedEvent, bool rescan, bool reportEntries) {
  dirent ** directoryContents = NULL;

  int resultCountOrError = scandir(
    mFullPath.c_str(),
    &directoryContents,
    NULL,
    alphasort
  );

  if (resultCountOrError < 0) {
    return false;
  }

  for (int i = 0; i < resultCountOrError; ++i) {
    std::string fileName = directoryContents[i]->d_name;

    if (
      fileName == "." ||
      fileName == ".."
    ) {
      continue;
    }

    unsigned char type = directoryContents[i]->d_type;
    if (mTree->isExcluded(createFullPath(mFullPath, fileName))) {
      if (type == DT_DIR || type == DT_LNK || type == DT_UNKNOWN) {
        mTree->setRejected(this, fileName, Rejected::BY_PATH);
      }
      continue;
    }
    if (rescan && mChildren->count(fileName) > 0) {
      continue;
    }

    if (createChild(fileName, emitCreatedEvent, !rescan || reportEntries) &&
      (type == DT_DIR || type == DT_LNK || type == DT_UNKNOWN)) {
      // It went away since it was listed, see InotifyTree::rescanIncomplete().
      mTree->markScanIncomplete(this);
    }
  }

  for (int i = 0; i < resultCountOrError; ++i) {
    free(directoryContents[i]);
  }

  free(directoryContents);
  return true;
}

InotifyTree::InotifyNode::~InotifyNode() {
  if (mTree->needsEntryRetry()) {
    auto &entries = mTree->mChangedEntries;
    entries.erase(std::remove_if(entries.begin(), entries.end(), [this](const ChangedEntry &entry) {
      return entry.parent == this;
    }), entries.end());
  }
  mTree->removeInode(mInodeNumber);
  mTree->mIncompleteScans.erase(this);
  mTree->mRejectedByPath.erase(this);
  mTree->mRejectedByAlias.erase(this);

  if (mWatchDescriptorInitialized) {
    mTree->removeNodeReferenceByWD(mWatchDescriptor, this);
  }

  for (auto i = mChildren->begin(); i != mChildren->end(); ++i) {
    delete i->second;
    i->second = NULL;
  }
  delete mChildren;
}

// Every child node is made here: by the scan of a new node, for a directory created or moved in after start(), and for
// a path updateExcludedPaths() includes again. Excluded paths, files and directories already watched (through a
// symlink) are skipped.
// Returns whether name went away before a node could watch it.
bool InotifyTree::InotifyNode::createChild(const std::string &name, EmitCreatedEvent emitCreatedEvent, bool reportEntry) {
  if (mFoundAgain) {
    // Not scanned until the marker of its pass, see InotifyTree::settleFound(): that scan finds name. A child found
    // again before it would share its pass, and the events of this node's old watch that tell where the child went
    // (into an excluded directory, say) are dropped until that marker.
    mFoundByExclusionUpdate = mFoundByExclusionUpdate || mTree->mRestoringExclusions;
    return false;
  }
  InotifyTree::Creating creating(mTree);
  std::string path = createFullPath(mFullPath, name);
  struct stat file;
  int lookupError = lstat(path.c_str(), &file) < 0 ? errno : 0;
  bool throughSymlink = lookupError == 0 && S_ISLNK(file.st_mode);
  bool directory = lookupError == 0 && (!throughSymlink || stat(path.c_str(), &file) == 0) && S_ISDIR(file.st_mode);
  if (directory && mTree->rejectsChild(this, name)) {
    return false;
  }
  if (emitCreatedEvent && reportEntry) {
    emitCreatedEvent(mFullPath, name);
  }
  if (lookupError != 0) {
    return lookupError == ENOENT || lookupError == ENOTDIR;
  }
  if (!directory) {
    return false;
  }

  // The inode is claimed before the child scans its contents, so a symlink back to it is not watched again. A
  // directory another node holds may have moved here, see InotifyTree::sight().
  if (!mTree->addInode(file.st_ino, this)) {
    mTree->sight(this, name, file, throughSymlink);
    return false;
  }

  // A directory renamed over an empty one replaces it. The replaced one's node would be left behind with its inode
  // claimed, and its IN_DELETE_SELF would remove the new child by name.
  removeChild(name);

  InotifyNode *child = new InotifyNode(
    mTree,
    mInotifyInstance,
    this,
    mFullPath,
    name,
    file.st_dev,
    file.st_ino,
    throughSymlink,
    emitCreatedEvent
  );

  if (child->isAlive()) {
    // Points the inode at the child, as the constructor does, so findNodeByPath finds the child and not this node.
    mTree->inodes[file.st_ino] = child;
    (*mChildren)[name] = child;
    mTree->directoryChanged({ file.st_dev, file.st_ino }, this);
    return false;
  }

  if (child->mRegistrationChanged) {
    mTree->deferChangedEntry({ this, name, emitCreatedEvent, mTree->mRestoringExclusions, mTree->mCurrentUpdate,
      mTree->mLastRemoval, 0 });
  }
  // Its destructor releases the inode.
  delete child;
  return lstat(path.c_str(), &file) < 0 && (errno == ENOENT || errno == ENOTDIR);
}

// The node at relativePath (names separated by '/') below this one, or NULL if one of them isn't watched.
InotifyTree::InotifyNode *InotifyTree::InotifyNode::findDescendant(const std::string &relativePath) {
  InotifyNode *node = this;
  size_t start = 0;
  while (node != NULL && start <= relativePath.size()) {
    size_t end = relativePath.find('/', start);
    if (end == std::string::npos) {
      end = relativePath.size();
    }
    auto child = node->mChildren->find(relativePath.substr(start, end - start));
    node = child == node->mChildren->end() ? NULL : child->second;
    start = end + 1;
  }
  return node;
}

void InotifyTree::InotifyNode::fixPaths() {
  std::string parentPath = mParent->getFullPath();
  std::string fullPath = createFullPath(parentPath, mName);

  if (fullPath == mFullPath) {
    return;
  }

  mFullPath = fullPath;

  for(auto i = mChildren->begin(); i != mChildren->end(); ++i) {
    i->second->fixPaths();
  }
}

dev_t InotifyTree::InotifyNode::getDevice() {
  return mDevice;
}

std::string InotifyTree::InotifyNode::getFullPath() {
  return mFullPath;
}

const std::string &InotifyTree::InotifyNode::getName() const {
  return mName;
}

bool InotifyTree::InotifyNode::isAlive() {
  return mAlive;
}

// Whether its path still leads to its directory, which a rename still queued may have changed.
bool InotifyTree::InotifyNode::leadsToItself() {
  return leadsTo(mFullPath, mThroughSymlink, mDevice, mInodeNumber);
}

bool InotifyTree::InotifyNode::isAtStaleLink() {
  return mThroughSymlink && !leadsToItself();
}

InotifyTree::InotifyNode *InotifyTree::InotifyNode::getParent() {
  return mParent;
}

ino_t InotifyTree::InotifyNode::getInodeNumber() {
  return mInodeNumber;
}

int InotifyTree::InotifyNode::getWatchDescriptor() {
  return mWatchDescriptorInitialized ? mWatchDescriptor : -1;
}

// Removes its watch, which makes the marker with that sequence number, and watches its path again: replaced is the old
// watch descriptor if its removal queued an IN_IGNORED. Whether it's still watched, which it isn't if its directory is
// gone, or a rename put another one with a watch at its path: their events handle it.
bool InotifyTree::InotifyNode::rewatch(uint64_t marker, int &replaced) {
  int old = mWatchDescriptor;
  auto byWatchDescriptor = mTree->mInotifyNodeByWatchDescriptor;
  byWatchDescriptor->erase(old);
  mTree->mQuarantined.erase(old);
  replaced = 0;
  if (inotify_rm_watch(mInotifyInstance, old) == 0) {
    mTree->mRemovedWatches[old] = { ++mTree->mLastRemoval, marker };
    replaced = old;
  }
  #ifdef NSFW_TEST_SLOW_1
    struct stat testFailure;
    if (stat((mTree->mWatchedPath + "/nsfw-test-fail-rewatch").c_str(), &testFailure) == 0) {
      errno = ENOMEM;
      mWatchDescriptor = -1;
    } else
  #endif
  {
    mWatchDescriptor = addWatch(ATTRIBUTES);
  }
  if (mWatchDescriptor == -1) {
    handleWatchFailure(errno);
  }
  bool owned = mWatchDescriptor != -1 && mTree->mDroppedWatches.count(mWatchDescriptor) == 0 &&
    byWatchDescriptor->count(mWatchDescriptor) == 0;
  if (!registrationMatchesPath() || !owned) {
    if (owned) {
      // A rename put another directory here. Retire its watch with the other dropped watches.
      mTree->mDroppedWatches[mWatchDescriptor] = { false };
    }
    mWatchDescriptorInitialized = false;
    return false;
  }
  mTree->addNodeReferenceByWD(mWatchDescriptor, this);
  return true;
}

// Whether it waits for the scan after its pass's marker, which the caller does now.
bool InotifyTree::InotifyNode::takeFoundAgain(bool &fromExclusionUpdate) {
  bool foundAgain = mFoundAgain;
  fromExclusionUpdate = mFoundByExclusionUpdate;
  mFoundAgain = false;
  mFoundByExclusionUpdate = false;
  return foundAgain;
}

bool InotifyTree::InotifyNode::foundByExclusionUpdate() const {
  return mFoundByExclusionUpdate;
}

// Adds this node's watch, removing dropped watches while the limit of watches is reached, see
// InotifyTree::freeDroppedWatch(). The watch descriptor, or -1 with errno set.
int InotifyTree::InotifyNode::addWatch(int attributes) {
  int watchDescriptor = inotify_add_watch(mInotifyInstance, mFullPath.c_str(), attributes);
  while (watchDescriptor == -1 && errno == ENOSPC) {
    if (!mTree->freeDroppedWatch()) {
      errno = ENOSPC;
      break;
    }
    watchDescriptor = inotify_add_watch(mInotifyInstance, mFullPath.c_str(), attributes);
  }
  return watchDescriptor;
}

void InotifyTree::InotifyNode::handleWatchFailure(int error) {
  if (error == ENOSPC && mParent != NULL && !mTree->mDroppedWatches.empty()) {
    mTree->mRetries.push_back({
      mParent->getWatchDescriptor(), mName, mDevice, mInodeNumber, mThroughSymlink, mTree->mRestoringExclusions
    });
  } else if (error == ENOSPC) {
    mTree->setError("Inotify limit reached");
  } else if (error == ENOMEM) {
    mTree->setError("Kernel out of memory");
  } else if (error == EBADF || error == EINVAL) {
    mTree->setError("Invalid file descriptor");
  }
}

bool InotifyTree::InotifyNode::registrationMatchesPath() {
  if (mWatchDescriptor == -1) return false;
  struct stat file;
  if (lstat(mFullPath.c_str(), &file) != 0) return false;
  bool link = S_ISLNK(file.st_mode);
  if ((link && stat(mFullPath.c_str(), &file) != 0) || !S_ISDIR(file.st_mode)) return false;

  auto owner = mTree->findNodeByWatchDescriptor(mWatchDescriptor);
  if (owner && owner->mDevice == file.st_dev && owner->mInodeNumber == file.st_ino) {
    if (mParent) mTree->sight(mParent, mName, file, link);
    return false;
  }
  mRegistrationChanged = owner || (link && !mThroughSymlink) ||
    file.st_dev != mDevice || file.st_ino != mInodeNumber;
  return !mRegistrationChanged;
}

bool InotifyTree::InotifyNode::inotifyInit() {
  if (mTree->hasErrored()) {
    mAlive = false;
    return false;
  }

  int attr = mParent != NULL
           ? ATTRIBUTES
           : ATTRIBUTES | IN_MOVE_SELF;

  mWatchDescriptor = addWatch(attr);
#ifdef NSFW_TEST_SLOW_1
  if (mWatchDescriptor != -1 && mName == "nsfw-test-changing-link" &&
    signalRegistration(mTree->mWatchedPath + ".registered")) {
    std::this_thread::sleep_for(std::chrono::milliseconds(500));
  }
#endif
  // A directory dropped from the tree whose watch isn't removed yet gets that watch back, with the events it queued
  // meanwhile, maybe outside the watched path or in an excluded directory. The root's watch is never a dropped one.
  auto dropped = mTree->mDroppedWatches.find(mWatchDescriptor);
  if (mWatchDescriptor != -1 && !registrationMatchesPath()) {
    if (!mTree->findNodeByWatchDescriptor(mWatchDescriptor) && dropped == mTree->mDroppedWatches.end()) {
      mTree->mDroppedWatches[mWatchDescriptor] = { false };
    }
    if (!mRegistrationChanged && mParent) mTree->markScanIncomplete(mParent);
    mAlive = false;
    return false;
  }
  if (mWatchDescriptor != -1 && mParent != NULL && dropped != mTree->mDroppedWatches.end()) {
    // Kept, with its events dropped until the marker the pass it's found in queues after every find of the pass,
    // and read after that marker, see InotifyTree::settleFound().
    mTree->mDroppedWatches.erase(dropped);
    mTree->mQuarantined[mWatchDescriptor] = InotifyTree::PASS_PENDING;
    mFoundAgain = true;
  }

  mAlive = (mWatchDescriptor != -1);

  if (!mAlive) {
    handleWatchFailure(errno);
    return false;
  }

  mWatchDescriptorInitialized = true;
  mTree->addNodeReferenceByWD(mWatchDescriptor, this);

  return mAlive;
}

void InotifyTree::InotifyNode::removeChild(std::string name) {
  auto child = mChildren->find(name);
  if (child != mChildren->end()) {
    delete child->second;
    child->second = NULL;
    mChildren->erase(child);
  }
}

void InotifyTree::InotifyNode::setName(std::string name) {
  mName = name;
  fixPaths();
}

void InotifyTree::InotifyNode::setParent(InotifyNode *newParent) {
  mParent = newParent;
}

std::string InotifyTree::InotifyNode::createFullPath(std::string parentPath, std::string name) {
  if (name == "") {
    return parentPath;
  }
  return parentPath + (parentPath == "/" ? "" : "/") + name;
}

InotifyTree::InotifyNode *InotifyTree::InotifyNode::pullChild(std::string name) {
  auto child = mChildren->find(name);
  if (child == mChildren->end()) {
    return NULL;
  }

  auto node = child->second;
  node->setParent(NULL);
  mChildren->erase(child);
  return node;
}

void InotifyTree::InotifyNode::takeChildAsName(InotifyNode *child, std::string name) {
  (*mChildren)[name] = child;
  child->setParent(this);
  child->setName(name);
}
