#include "../../includes/osx/FSEventsService.h"
#include <algorithm>
#include <unordered_set>
#include <iostream>
#include <cerrno>
#include <cstring>
#include <iterator>
#include <sys/attr.h>

namespace {
  // How many paths FSEventsService remembers it reported gone.
  const size_t kGoneLimit = 4096;

  // APFS can flag a clone separately from a creation. It can also flag the clone's source.
  const FSEventStreamEventFlags kCreatedFlags = kFSEventStreamEventFlagItemCreated | kFSEventStreamEventFlagItemCloned;

  const FSEventStreamEventFlags kModifiedFlags = kFSEventStreamEventFlagItemModified |
    kFSEventStreamEventFlagItemInodeMetaMod | kFSEventStreamEventFlagItemFinderInfoMod |
    kFSEventStreamEventFlagItemChangeOwner | kFSEventStreamEventFlagItemXattrMod;
}

FSEventsService::FSEventsService(std::shared_ptr<EventQueue> queue, std::string path, const std::vector<std::string> &excludedPaths,
  bool recursive):
  mPath(path), mQueue(queue), mRootChanged(false), mRecursive(recursive) {
  mCaseSensitive = pathconf(path.c_str(), _PC_CASE_SENSITIVE) > 0;
  mExcludedPaths = createExcludedPaths(excludedPaths);
  mRootExcluded = excludesRoot(mExcludedPaths);
  clock_gettime(CLOCK_REALTIME, &mStartTime);
  mStartEventId = FSEventsGetCurrentEventId();

  mRunLoop = new RunLoop(this, path);

  if (!mRunLoop->isLooping()) {
    delete mRunLoop;
    mRunLoop = NULL;
    return;
  }
}

FSEventsService::~FSEventsService() {
  // Stops the run loop thread, the only other reader of mExcludedPaths.
  if (mRunLoop != NULL) {
    delete mRunLoop;
  }
  releaseExcludedPaths(mExcludedPaths);
}

std::vector<CFStringRef> FSEventsService::createExcludedPaths(const std::vector<std::string> &excludedPaths) const {
  std::vector<CFStringRef> excludedPathRefs;
  for (const std::string &excludedPath : excludedPaths) {
    CFStringRef excludedPathRef = CFStringCreateWithCString(NULL, excludedPath.c_str(), kCFStringEncodingUTF8);
    if (excludedPathRef != NULL) {
      excludedPathRefs.push_back(excludedPathRef);
    }
  }
  return excludedPathRefs;
}

void FSEventsService::releaseExcludedPaths(const std::vector<CFStringRef> &excludedPaths) {
  for (CFStringRef excludedPath : excludedPaths) {
    CFRelease(excludedPath);
  }
}

bool FSEventsService::excludesRoot(const std::vector<CFStringRef> &excludedPaths) const {
  CFStringRef root = CFStringCreateWithCString(NULL, mPath.c_str(), kCFStringEncodingUTF8);
  if (root == NULL) return false;
  const CFStringCompareFlags flags = pathCompareFlags();
  bool excluded = false;
  for (CFStringRef excludedPath : excludedPaths) {
    if (CFStringCompare(root, excludedPath, flags) == kCFCompareEqualTo) {
      excluded = true;
      break;
    }
  }
  CFRelease(root);
  return excluded;
}

void FSEventsServiceCallback(
  ConstFSEventStreamRef streamRef,
  void *clientCallBackInfo,
  size_t numEvents,
  void *eventPaths,
  const FSEventStreamEventFlags eventFlags[],
  const FSEventStreamEventId eventIds[]
) {
  FSEventsService *eventsService = (FSEventsService *)clientCallBackInfo;
  CFArrayRef eventData = (CFArrayRef)eventPaths;
  std::vector<FSEventsService::Event> events;
  events.reserve(numEvents);
  for (size_t i = 0; i < numEvents && !eventsService->mRootChanged; ++i) {
    const FSEventStreamEventFlags rootChanged = kFSEventStreamEventFlagRootChanged;
    if (eventIds[i] == 0 && (eventFlags[i] & rootChanged) == rootChanged) {
      eventsService->mRootChanged = true;
      eventsService->mQueue->wake(EventQueue::WAKE_ERROR);
      break;
    }
    FSEventsService::Event event;
    if (!FSEventsService::readEventData(CFArrayGetValueAtIndex(eventData, i), event.path, event.inode)) {
      continue;
    }
    if (eventsService->isOutsideChildren(event.path.c_str())) {
      continue;
    }
    event.flags = eventFlags[i];
    event.id = eventIds[i];
    // A stream that starts now can still deliver the latest changes made before it started. Id 0 is about the
    // stream or a volume, not a change of an entry.
    if (event.id <= eventsService->mStartEventId) {
      continue;
    }
    if (eventsService->isExcluded(event.path)) {
      eventsService->noteExcluded(event);
      continue;
    }
    events.push_back(std::move(event));
  }
  eventsService->handle(std::move(events));
}

namespace {
  bool toUTF8(CFStringRef string, std::string &utf8) {
    const char *direct = CFStringGetCStringPtr(string, kCFStringEncodingUTF8);
    if (direct != NULL) {
      utf8 = direct;
      return true;
    }
    const CFIndex size = CFStringGetMaximumSizeForEncoding(CFStringGetLength(string), kCFStringEncodingUTF8) + 1;
    std::vector<char> buffer(size);
    if (!CFStringGetCString(string, buffer.data(), size, kCFStringEncodingUTF8)) {
      return false;
    }
    utf8 = buffer.data();
    return true;
  }

  // What an entry is for the filter, which needs it once the entry is gone. A symlink counts as a directory, which it
  // most often leads to, as the filter follows it when it can, so its removal is judged like its creation was.
  PathFilter::Kind kindFromMode(mode_t mode) {
    if (S_ISDIR(mode) || S_ISLNK(mode)) {
      return PathFilter::Kind::DIRECTORY;
    }
    return PathFilter::Kind::FILE;
  }

  // As kindFromMode(), unless the flags coalesce two entries.
  PathFilter::Kind kindOf(FSEventStreamEventFlags flags) {
    const bool isDirectory = (flags & kFSEventStreamEventFlagItemIsDir) != 0;
    const bool isFile = (flags & kFSEventStreamEventFlagItemIsFile) != 0;
    const bool isSymlink = (flags & kFSEventStreamEventFlagItemIsSymlink) != 0;
    return isSymlink ? PathFilter::Kind::DIRECTORY : isDirectory == isFile ?
      PathFilter::Kind::UNKNOWN : (isDirectory ? PathFilter::Kind::DIRECTORY : PathFilter::Kind::FILE);
  }
}

// data: an extended data dictionary, or the bare path when the system doesn't give one.
bool FSEventsService::readEventData(const void *data, std::string &path, uint64_t &inode) {
  inode = 0;
  if (data == NULL) {
    return false;
  }
  CFStringRef pathRef = NULL;
  if (CFGetTypeID(data) == CFDictionaryGetTypeID()) {
    CFDictionaryRef dictionary = (CFDictionaryRef)data;
    const void *pathValue = CFDictionaryGetValue(dictionary, kFSEventStreamEventExtendedDataPathKey);
    if (pathValue != NULL && CFGetTypeID(pathValue) == CFStringGetTypeID()) {
      pathRef = (CFStringRef)pathValue;
    }
    const void *inodeValue = CFDictionaryGetValue(dictionary, kFSEventStreamEventExtendedFileIDKey);
    int64_t fileID;
    if (
      inodeValue != NULL && CFGetTypeID(inodeValue) == CFNumberGetTypeID() &&
      CFNumberGetValue((CFNumberRef)inodeValue, kCFNumberSInt64Type, &fileID)
    ) {
      inode = (uint64_t)fileID;
    }
  } else if (CFGetTypeID(data) == CFStringGetTypeID()) {
    pathRef = (CFStringRef)data;
  }
  return pathRef != NULL && toUTF8(pathRef, path);
}

void FSEventsService::handle(std::vector<Event> events) {
  std::vector<RenameSide> sides(events.size());
  bool anyRenamed = false;
  for (size_t i = 0; i < events.size(); ++i) {
    if ((events[i].flags & kFSEventStreamEventFlagItemRenamed) == 0) {
      continue;
    }
    anyRenamed = true;
    RenameSide &side = sides[i];
    side.renamed = true;
    struct stat file;
    if (lstat(events[i].path.c_str(), &file) == 0) {
      side.exists = true;
      side.inode = file.st_ino;
      side.present = events[i].inode == 0 || events[i].inode == side.inode;
      side.kind = kindFromMode(file.st_mode);
      side.device = file.st_dev;
    } else if (events[i].inode != 0) {
      side.hardLinked = side.present && !S_ISDIR(file.st_mode) && file.st_nlink > 1;
      // Only an entry FSEvents told the inode of is looked for by volume. The nearest directory still there: one gone
      // too was on the same volume as its parent, unless it was a mount point.
      std::string directory = events[i].path, name;
      bool found = false;
      while (!found && directory != "/") {
        splitFilePath(directory, name, std::string(directory));
        found = lstat(directory.c_str(), &file) == 0;
      }
      side.device = found ? file.st_dev : 0;
    }
  }
  // The events of a path in the batch are handled together, where the first one is, or by a rename side of the path
  // that follows them: their flags pile up anyway.
  std::unordered_map<std::string, size_t> first;
  for (size_t i = 0; i < events.size(); ++i) {
    if (sides[i].renamed) {
      auto earlier = first.find(events[i].path);
      if (earlier != first.end()) {
        events[i].flags |= events[earlier->second].flags;
        sides[earlier->second].repeated = true;
        first.erase(earlier);
      }
      continue;
    }
    auto found = first.emplace(events[i].path, i);
    if (!found.second) {
      events[found.first->second].flags |= events[i].flags;
      sides[i].repeated = true;
    }
  }
  std::vector<bool> splitRenames(events.size(), false);
  if (anyRenamed) {
    pairRenames(events, sides);
    // What took the place an entry left is reported by its own side when it has one, as the destination of another
    // rename in the batch, say.
    std::unordered_set<std::string> held;
    for (size_t i = 0; i < events.size(); ++i) {
      if (sides[i].present && !sides[i].repeated) {
        held.insert(events[i].path);
      }
    }
    for (size_t i = 0; i < events.size(); ++i) {
      sides[i].replaced = sides[i].exists && !sides[i].present && held.count(events[i].path) != 0;
    }

    // Collapsing a chain can hide the temporary name that kept another rename's source intact.
    std::unordered_map<std::string, size_t> sources;
    for (size_t i = 0; i < events.size(); ++i) {
      if (sides[i].from) {
        sources.emplace(events[i].path, i);
      }
    }
    for (size_t i = 0; i < events.size(); ++i) {
      if (!sides[i].from) {
        continue;
      }
      const size_t destination = sides[i].partner;
      const auto other = sources.find(events[destination].path);
      if (other != sources.end() && other->second > i) {
        splitRenames[i] = splitRenames[destination] = true;
        splitRenames[other->second] = splitRenames[sides[other->second].partner] = true;
      }
    }
    for (size_t i = 0; i < events.size(); ++i) {
      if (splitRenames[i] && sides[i].from && !mRootChanged) {
        remove(events[i].path, sides[sides[i].partner].kind, events[i].inode);
      }
    }
  }

  for (size_t i = 0; i < events.size(); ++i) {
    const Event &event = events[i];
    const RenameSide &side = sides[i];
    if (!side.renamed) {
      if (!side.repeated) {
        classify(event, kindOf(event.flags));
      }
    } else if (mRootChanged) {
      // The paths no longer lead where they did.
      continue;
    } else if (splitRenames[i]) {
      if (!side.from) {
        create(event.path, side.inode, side.kind);
      }
    } else if (side.repeated) {
      // A later side at the same path speaks for it.
      continue;
    } else if (side.transient) {
      passThrough(event, side);
    } else if (side.partner < 0) {
      classify(event, kindOf(event.flags));
    } else if (side.from) {
      const RenameSide &to = sides[side.partner];
      rename(event.path, events[side.partner].path, to.inode, to.kind);
      // Something took the place the entry left.
      if (side.exists && !side.replaced && side.inode != to.inode) {
        create(event.path, side.inode, side.kind);
      }
    }
  }
}

void FSEventsService::pairSides(std::vector<RenameSide> &sides, size_t from, size_t to) {
  sides[from].partner = to;
  sides[from].from = true;
  sides[to].partner = from;
  sides[to].from = false;
}

// FSEvents reports a rename as one event for each side, the source then the destination with the next event id, both
// about the same inode. The flags of both can hold earlier changes too, and the sides can be in different directories.
// An entry renamed more than once in the batch has a side for each place it passed through, in event id order.
void FSEventsService::pairRenames(const std::vector<Event> &events, std::vector<RenameSide> &sides) {
  // The sides about each entry, by its volume and inode: inodes are only unique within a volume.
  std::map<std::pair<dev_t, uint64_t>, std::vector<size_t>> sidesByEntry;
  std::unordered_map<FSEventStreamEventId, size_t> presentById;
  for (size_t i = 0; i < events.size(); ++i) {
    if (!sides[i].renamed) {
      continue;
    }
    if (sides[i].present) {
      presentById[events[i].id] = i;
    }
    const uint64_t inode = sides[i].present ? sides[i].inode : events[i].inode;
    if (inode != 0) {
      sidesByEntry[{ sides[i].device, inode }].push_back(i);
    }
  }
  auto byId = [&events](size_t a, size_t b) { return events[a].id < events[b].id; };

  for (auto &entry : sidesByEntry) {
    const uint64_t inode = entry.first.second;
    std::vector<size_t> &candidates = entry.second;
    std::sort(candidates.begin(), candidates.end(), byId);
    // One side speaks for each place the entry was at, its latest, in the order the entry first was there.
    const bool hardLinked = std::any_of(candidates.begin(), candidates.end(), [&](size_t i) {
      return sides[i].hardLinked || (events[i].flags & kFSEventStreamEventFlagItemIsHardlink) != 0;
    });
    if (hardLinked) {
      // Paths to a hard-linked inode can be renamed independently. Only pair adjacent sides of this inode.
      for (size_t j = 1; j < candidates.size(); ++j) {
        const size_t from = candidates[j - 1], to = candidates[j];
        uint64_t reported;
        const bool knownSource = lookUpReported(events[from].path, reported) && reported == inode;
        if (!sides[from].present && sides[to].present && events[to].id == events[from].id + 1 &&
          (candidates.size() == 2 || knownSource)) {
          pairSides(sides, from, to);
        }
      }
      continue;
    }
    std::vector<size_t> places;
    std::unordered_map<std::string, size_t> placeOf;
    for (size_t i : candidates) {
      auto found = placeOf.find(events[i].path);
      if (found == placeOf.end()) {
        placeOf.emplace(events[i].path, places.size());
        places.push_back(i);
      } else {
        sides[places[found->second]].repeated = true;
        places[found->second] = i;
      }
    }
    std::vector<size_t> present;
    for (size_t i : places) {
      if (sides[i].present) {
        present.push_back(i);
      }
    }
    // Two places hold the entry: not a rename of it.
    if (present.size() > 1) {
      continue;
    }
    // Where the entry was before the batch: where it was reported, else the first place it was at.
    size_t source = places.front();
    for (size_t i : places) {
      uint64_t reported = 0;
      if (lookUpReported(events[i].path, reported) && reported == inode) {
        source = i;
        break;
      }
    }
    for (size_t i : places) {
      sides[i].transient = !sides[i].present && i != source;
    }
    if (present.empty()) {
      continue;
    }
    // A single place that holds the entry is where it arrived, from outside the batch or the watched path.
    if (source == present.front() && places.size() > 1) {
      sides[source].returned = true;
    } else if (source != present.front()) {
      pairSides(sides, source, present.front());
    }
  }

  // FSEvents didn't tell what the side that left is about: the side with the next event id is its destination, unless
  // that entry was already reported where it is.
  for (size_t i = 0; i < events.size(); ++i) {
    if (!sides[i].renamed || sides[i].present || events[i].inode != 0) {
      continue;
    }
    auto found = presentById.find(events[i].id + 1);
    uint64_t reported = 0;
    if (
      found != presentById.end() && sides[found->second].partner < 0 &&
      !(lookUpReported(events[found->second].path, reported) && reported == sides[found->second].inode)
    ) {
      pairSides(sides, i, found->second);
    }
  }
}

// A place an entry renamed again within the batch passed through: it was never reported there, so only what was
// there before or is there now is news.
void FSEventsService::passThrough(const Event &event, const RenameSide &side) {
  uint64_t reported = 0;
  const bool known = lookUpReported(event.path, reported);
  if (side.exists && !side.replaced && !(known && reported == side.inode)) {
    create(event.path, side.inode, side.kind);
  } else if (!side.exists && (known || (event.flags & kFSEventStreamEventFlagItemRemoved) != 0)) {
    remove(event.path, kindOf(event.flags));
  }
}

bool FSEventsService::isExcluded(std::string filePath) {
  std::string directory, name;
  splitFilePath(directory, name, filePath);
  CFStringRef fileDirectory = CFStringCreateWithCString(
    NULL,
    directory.c_str(),
    kCFStringEncodingUTF8
  );
  if (fileDirectory == NULL) {
    return false;
  }
  CFIndex lenDirectory = CFStringGetLength(fileDirectory);
  const CFStringCompareFlags flags = pathCompareFlags();
  bool excluded = false;
  {
    std::lock_guard<std::mutex> lock(mExcludedPathsLock);
    if (mRootExcluded) {
      CFRelease(fileDirectory);
      return true;
    }
    for (CFStringRef excludedPath : mExcludedPaths) {
      CFIndex lenExclude = CFStringGetLength(excludedPath);
      if (
        lenExclude == 0 ||
        lenDirectory < lenExclude ||
        CFStringCompareWithOptions(fileDirectory, excludedPath, CFRangeMake(0, lenExclude), flags) != kCFCompareEqualTo
      ) {
        continue;
      }
      // Only whole path components match: excluding /a/foo must not exclude /a/foobar.
      if (
        lenDirectory == lenExclude ||
        CFStringGetCharacterAtIndex(excludedPath, lenExclude - 1) == '/' ||
        CFStringGetCharacterAtIndex(fileDirectory, lenExclude) == '/'
      ) {
        excluded = true;
        break;
      }
    }
  }
  CFRelease(fileDirectory);
  return excluded;
}

// Whether a non-recursive watcher drops the event of filePath: the watched path itself or deeper than its entries.
bool FSEventsService::isOutsideChildren(const char *filePath) const {
  if (mRecursive) {
    return false;
  }
  const size_t prefix = mPath == "/" ? 1 : mPath.size() + 1;
  const size_t length = strlen(filePath);
  return length <= prefix || memchr(filePath + prefix, '/', length - prefix) != NULL;
}

CFStringCompareFlags FSEventsService::pathCompareFlags() const {
  return mCaseSensitive ? kCFCompareLocalized : kCFCompareLocalized | kCFCompareCaseInsensitive;
}

bool FSEventsService::isSinceStart(const struct timespec &time) const {
  if (time.tv_sec != mStartTime.tv_sec) {
    return time.tv_sec > mStartTime.tv_sec;
  }
  // A volume that keeps whole seconds, like HFS+, can't tell within the second the stream started.
  return time.tv_nsec == 0 || time.tv_nsec >= mStartTime.tv_nsec;
}

bool FSEventsService::readAddedTime(const std::string &path, uint64_t inode, struct timespec &added) const {
  struct attrlist request{};
  request.bitmapcount = ATTR_BIT_MAP_COUNT;
  request.commonattr = ATTR_CMN_FILEID | ATTR_CMN_ADDEDTIME;
  uint32_t length;
  uint64_t found;
  // Attribute values are packed on four-byte boundaries, even for the inode and timespec.
  char buffer[sizeof(length) + sizeof(found) + sizeof(added)];
  if (getattrlist(path.c_str(), &request, buffer, sizeof(buffer), FSOPT_NOFOLLOW) != 0) {
    return false;
  }
  memcpy(&length, buffer, sizeof(length));
  if (length != sizeof(buffer)) {
    return false;
  }
  memcpy(&found, buffer + sizeof(length), sizeof(found));
  memcpy(&added, buffer + sizeof(length) + sizeof(found), sizeof(added));
  return found == inode && added.tv_sec != 0;
}

bool FSEventsService::appearedWhileWatching(const Event &event, const struct stat &file) const {
  if (isSinceStart(file.st_birthtimespec)) {
    return true;
  }
  if ((event.flags & kCreatedFlags) == 0 || (event.inode != 0 && event.inode != (uint64_t)file.st_ino)) {
    return false;
  }
  // Backdating a copy's modification time can also backdate its birth time. Its addition time can still
  // distinguish the destination from an older source that also received a clone flag.
  struct timespec added;
  if (readAddedTime(event.path, file.st_ino, added)) {
    return isSinceStart(added);
  }
  return isSinceStart(file.st_ctimespec) && !isSinceStart(file.st_mtimespec);
}

// What an event that isn't part of a rename stands for. Its flags can hold those of earlier changes to the path,
// reported or not, so they only tell that something changed there: what the watcher last reported at the path, and
// whether the entry there now was born before watching, tell what.
void FSEventsService::classify(const Event &event, PathFilter::Kind kind) {
  const bool isCreated = (event.flags & kCreatedFlags) != 0;
  const bool isRemoved = (event.flags & kFSEventStreamEventFlagItemRemoved) != 0;
  const bool isModified = (event.flags & kModifiedFlags) != 0;
  if (isModified && !isCreated && !isRemoved) {
    modify(event.path);
    return;
  }

  uint64_t reported = 0;
  const bool known = lookUpReported(event.path, reported);
  struct stat file;
  if (lstat(event.path.c_str(), &file) != 0) {
    if ((errno != ENOENT && errno != ENOTDIR) || wasReportedGone(event)) {
      return;
    }
    // The name may already have changed when a creation arrives. Wait for evidence of removal or rename.
    const bool hasCreation = (event.flags & kFSEventStreamEventFlagItemCreated) != 0;
    if (!known && !isRemoved) {
      if (hasCreation) {
        rememberMissing(event.path, event.inode, true);
      }
      return;
    }
    const auto missing = mGone.find(event.path);
    const bool wasCreated = missing != mGone.end() && missing->second.pendingCreation &&
      (event.inode == 0 || missing->second.inode == 0 || event.inode == missing->second.inode);
    if ((hasCreation || wasCreated) && !known) {
      create(event.path, 0, kind);
    }
    remove(event.path, kind, known ? reported : event.inode);
    return;
  }
  if (known && reported == (uint64_t)file.st_ino) {
    if (isModified || (!isCreated && !isRemoved)) {
      modify(event.path);
    }
    return;
  }
  if (!known && !appearedWhileWatching(event, file)) {
    if (isModified || isRemoved || (event.flags & kFSEventStreamEventFlagItemCreated) != 0) {
      modify(event.path);
    }
    return;
  }
  if (known && isRemoved) {
    remove(event.path, PathFilter::Kind::UNKNOWN);
  }
  create(event.path, file.st_ino);
}

// Notes an entry that appeared or left while excluded as if reported: once included again, what it already holds is
// not reported, so a change to it is a modification.
void FSEventsService::noteExcluded(const Event &event) {
  const FSEventStreamEventFlags appearedOrLeft = kCreatedFlags |
    kFSEventStreamEventFlagItemRemoved | kFSEventStreamEventFlagItemRenamed;
  if ((event.flags & appearedOrLeft) == 0) {
    return;
  }
  struct stat file;
  if (lstat(event.path.c_str(), &file) != 0) {
    noteReported(event.path, 0);
  } else if ((event.flags & kFSEventStreamEventFlagItemRenamed) != 0 || appearedWhileWatching(event, file)) {
    noteReported(event.path, file.st_ino);
  }
}

void FSEventsService::create(const std::string &path, uint64_t inode, PathFilter::Kind kind) {
  dispatch(CREATED, path, kind);
  noteReported(path, inode);
}

void FSEventsService::dispatch(EventType action, std::string path, PathFilter::Kind kind) {
  std::string directory, name;

  splitFilePath(directory, name, path);

  mQueue->enqueue(action, directory, name, "", "", kind);
}

std::string FSEventsService::getError() {
  if (mRootChanged) {
    return "Service shutdown: root path changed (renamed or deleted)";
  }
  return "Service shutdown unexpectedly";
}

bool FSEventsService::hasErrored() {
  return !isWatching() || mRootChanged;
}

void FSEventsService::runLoopExited() {
  mQueue->wake(EventQueue::WAKE_ERROR);
}

bool FSEventsService::isWatching() {
  return mRunLoop != NULL && mRunLoop->isLooping();
}

void FSEventsService::modify(std::string path) {
  dispatch(MODIFIED, path);
}

void FSEventsService::remove(std::string path, PathFilter::Kind kind, uint64_t inode) {
  dispatch(DELETED, path, kind);
  noteGone(path, inode);
}

void FSEventsService::noteGone(const std::string &path, uint64_t inode) {
  noteReported(path, 0);
  rememberMissing(path, inode, false);
}

void FSEventsService::rememberMissing(const std::string &path, uint64_t inode, bool pendingCreation) {
  forgetGone(path);
  mGoneOrder.push_back(path);
  mGone.emplace(path, GoneEntry{inode, pendingCreation, std::prev(mGoneOrder.end())});
  if (mGoneOrder.size() > kGoneLimit) {
    forgetGone(mGoneOrder.front());
  }
}

void FSEventsService::forgetGone(const std::string &path) {
  auto found = mGone.find(path);
  if (found != mGone.end()) {
    mGoneOrder.erase(found->second.position);
    mGone.erase(found);
  }
}

bool FSEventsService::wasReportedGone(const Event &event) const {
  auto found = mGone.find(event.path);
  return found != mGone.end() && !found->second.pendingCreation &&
    (found->second.inode == 0 || event.inode == 0 || found->second.inode == event.inode);
}

bool FSEventsService::lookUpReported(const std::string &path, uint64_t &inode) {
  auto found = mReported.find(path);
  if (found == mReported.end()) {
    return false;
  }
  inode = found->second;
  return true;
}

void FSEventsService::noteReported(const std::string &path, uint64_t inode) {
  if (inode != 0) {
    mReported[path] = inode;
    forgetGone(path);
    return;
  }
  // What it held went with it. '0' follows '/'.
  mReported.erase(path);
  mReported.erase(mReported.lower_bound(path + "/"), mReported.lower_bound(path + "0"));
}

void FSEventsService::rename(const std::string &from, const std::string &to, uint64_t inode, PathFilter::Kind kind) {
  std::string fromDirectory, fromName, toDirectory, toName;
  splitFilePath(fromDirectory, fromName, from);
  splitFilePath(toDirectory, toName, to);
  mQueue->enqueue(RENAMED, fromDirectory, fromName, toDirectory, toName, kind);
  // What a directory holds moves along with it, over what the destination held.
  noteReported(to, 0);
  const std::vector<std::pair<std::string, uint64_t>> held(
    mReported.lower_bound(from + "/"), mReported.lower_bound(from + "0"));
  noteGone(from, inode);
  for (const auto &entry : held) {
    mReported[to + entry.first.substr(from.size())] = entry.second;
  }
  noteReported(to, inode);
}

void FSEventsService::splitFilePath(std::string &directory, std::string &name, const std::string &path) {
  if (path.length() == 1 && path[0] == '/') {
    directory = "/";
    name = "";
  } else {
    uint32_t location = path.find_last_of("/");
    directory = (location == 0) ? "/" : path.substr(0, location);
    name = path.substr(location + 1);
  }
}

void FSEventsService::updateExcludedPaths(const std::vector<std::string> &excludedPaths) {
  std::vector<CFStringRef> excludedPathRefs = createExcludedPaths(excludedPaths);
  {
    std::lock_guard<std::mutex> lock(mExcludedPathsLock);
    mExcludedPaths.swap(excludedPathRefs);
    mRootExcluded = excludesRoot(mExcludedPaths);
  }
  // Now holds the previous list, which the run loop thread can no longer reach.
  releaseExcludedPaths(excludedPathRefs);
}
