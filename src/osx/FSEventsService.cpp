#include "../../includes/osx/FSEventsService.h"
#include <algorithm>
#include <unordered_set>
#include <iostream>
#include <cerrno>
#include <climits>
#include <cstring>
#include <iterator>
#include <unistd.h>
#include <sys/attr.h>
#include <sys/fsgetpath.h>
#include <sys/mount.h>

namespace {
  // How many paths FSEventsService remembers it reported gone.
  const size_t kGoneLimit = 4096;

  // APFS can flag a clone separately from a creation. It can also flag the clone's source.
  const FSEventStreamEventFlags kCreatedFlags = kFSEventStreamEventFlagItemCreated | kFSEventStreamEventFlagItemCloned;

  const FSEventStreamEventFlags kModifiedFlags = kFSEventStreamEventFlagItemModified |
    kFSEventStreamEventFlagItemInodeMetaMod | kFSEventStreamEventFlagItemFinderInfoMod |
    kFSEventStreamEventFlagItemChangeOwner | kFSEventStreamEventFlagItemXattrMod;
}

FSEventsService::FSEventsService(std::shared_ptr<EventQueue> queue, std::string path,
  const std::vector<std::string> &excludedPaths, std::shared_ptr<PathFilter> filter, bool recursive):
  mPath(path), mQueue(queue), mFilter(filter), mRootChanged(false), mRecursive(recursive), mDevice(0), mCanLocate(false) {
  mCaseSensitive = pathconf(path.c_str(), _PC_CASE_SENSITIVE) > 0;
  mExcludedPaths = createExcludedPaths(excludedPaths);
  mRootExcluded = excludesRoot(mExcludedPaths);
  clock_gettime(CLOCK_REALTIME, &mStartTime);
  mStartEventId = FSEventsGetCurrentEventId();

  // Use the same spelling for the watched path and for entries looked up by inode.
  struct statfs volume;
  struct stat root;
  if (statfs(path.c_str(), &volume) == 0 && stat(path.c_str(), &root) == 0) {
    mVolume = volume.f_fsid;
    mDevice = root.st_dev;
    char buffer[PATH_MAX];
    const ssize_t length = fsgetpath(buffer, sizeof(buffer), &mVolume, root.st_ino);
    if (length > 0) {
      mVolumePath.assign(buffer, strnlen(buffer, (size_t)length));
      mCanLocate = true;
    }
  }

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
    mStopping = true;
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

  // What the entry itself is, as events report it (see the kind of an Event in Queue.h): a symlink is not a directory.
  PathFilter::Kind reportedFromMode(mode_t mode) {
    return S_ISDIR(mode) ? PathFilter::Kind::DIRECTORY : PathFilter::Kind::FILE;
  }

  bool equalIgnoringCase(const std::string &first, const std::string &second) {
    CFStringRef firstRef = CFStringCreateWithCString(NULL, first.c_str(), kCFStringEncodingUTF8);
    CFStringRef secondRef = CFStringCreateWithCString(NULL, second.c_str(), kCFStringEncodingUTF8);
    const bool equal = firstRef != NULL && secondRef != NULL &&
      CFStringCompare(firstRef, secondRef, kCFCompareCaseInsensitive | kCFCompareNonliteral) == kCFCompareEqualTo;
    if (firstRef != NULL) CFRelease(firstRef);
    if (secondRef != NULL) CFRelease(secondRef);
    return equal;
  }
}

// As kindsFromMode(), unless the flags coalesce two entries: the filter's kind then, the reported one when they mix a
// directory with something else.
FSEventsService::Kinds FSEventsService::kindsOf(FSEventStreamEventFlags flags) {
  const bool isDirectory = (flags & kFSEventStreamEventFlagItemIsDir) != 0;
  const bool isFile = (flags & kFSEventStreamEventFlagItemIsFile) != 0;
  const bool isSymlink = (flags & kFSEventStreamEventFlagItemIsSymlink) != 0;
  const PathFilter::Kind filter = isSymlink ? PathFilter::Kind::DIRECTORY : isDirectory == isFile ?
    PathFilter::Kind::UNKNOWN : (isDirectory ? PathFilter::Kind::DIRECTORY : PathFilter::Kind::FILE);
  const PathFilter::Kind reported = isDirectory == (isFile || isSymlink) ?
    PathFilter::Kind::UNKNOWN : (isDirectory ? PathFilter::Kind::DIRECTORY : PathFilter::Kind::FILE);
  return { filter, reported };
}

// The reported kind the flags tell, else what is at path now.
PathFilter::Kind FSEventsService::reportedKindAt(FSEventStreamEventFlags flags, const std::string &path) {
  const PathFilter::Kind kind = kindsOf(flags).reported;
  struct stat file;
  if (kind != PathFilter::Kind::UNKNOWN || lstat(path.c_str(), &file) != 0) {
    return kind;
  }
  return reportedFromMode(file.st_mode);
}

// The filter needs a type after the entry disappears. A symlink can still be a directory to the filter; what is
// reported is the symlink itself.
FSEventsService::Kinds FSEventsService::kindsFromMode(mode_t mode) {
  const PathFilter::Kind filter = S_ISDIR(mode) || S_ISLNK(mode) ? PathFilter::Kind::DIRECTORY : PathFilter::Kind::FILE;
  return { filter, reportedFromMode(mode) };
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
      side.kinds = kindsFromMode(file.st_mode);
      side.isDirectory = S_ISDIR(file.st_mode);
      side.device = file.st_dev;
      side.hardLinked = side.present && !S_ISDIR(file.st_mode) && file.st_nlink > 1;
    } else if (events[i].inode != 0) {
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
        const Event &previous = events[earlier->second];
        if (previous.inode == 0 || events[i].inode == 0 || previous.inode == events[i].inode) {
          sides[i].creationObserved = (previous.flags & kFSEventStreamEventFlagItemCreated) != 0 &&
            (previous.flags & kFSEventStreamEventFlagItemRemoved) == 0;
          events[i].flags |= previous.flags;
          sides[earlier->second].repeated = true;
        }
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
        if (sides[i].creationObserved) {
          rememberMissing(events[i].path, events[i].inode, true);
        }
        remove(events[i].path, sides[sides[i].partner].kinds, events[i].inode);
      }
    }
  }

  for (size_t i = 0; i < events.size(); ++i) {
    const Event &event = events[i];
    const RenameSide &side = sides[i];
    if (side.creationObserved && !side.repeated && !splitRenames[i] && !mRootChanged) {
      rememberMissing(event.path, event.inode, true);
    }
    if (!side.renamed) {
      if (!side.repeated) {
        classify(event, kindsOf(event.flags));
      }
    } else if (mRootChanged) {
      // The paths no longer lead where they did.
      continue;
    } else if (splitRenames[i]) {
      if (!side.from) {
        appear(event.path, side);
      }
    } else if (side.repeated) {
      // A later side at the same path speaks for it.
      continue;
    } else if (side.transient) {
      passThrough(event, side);
    } else if (side.partner < 0 && side.present) {
      arrive(event, side);
    } else if (side.partner < 0) {
      depart(event, side);
    } else if (side.from) {
      const RenameSide &to = sides[side.partner];
      rename(event.path, events[side.partner].path, to.inode, to.kinds);
      // Something took the place the entry left.
      if (side.exists && !side.replaced && side.inode != to.inode) {
        appear(event.path, side);
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
    // One side speaks for each place the entry was at, its latest, in the order the entry first was there.
    std::vector<size_t> places;
    std::unordered_map<std::string, size_t> placeOf;
    for (size_t i : candidates) {
      auto found = placeOf.find(events[i].path);
      if (found == placeOf.end()) {
        placeOf.emplace(events[i].path, places.size());
        places.push_back(i);
      } else {
        RenameSide &previous = sides[places[found->second]];
        sides[i].creationObserved = sides[i].creationObserved || previous.creationObserved;
        previous.repeated = true;
        places[found->second] = i;
      }
    }
    std::vector<size_t> present;
    for (size_t i : places) {
      if (sides[i].present) {
        present.push_back(i);
      }
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
    if (present.size() > 1) {
      pairCaseRenames(events, present, source, sides);
      continue;
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

// On a case-insensitive volume, the old name of a rename that only changes its case still leads to the entry, so
// several places hold it: the destination is the one spelled as the volume stores the name. Elsewhere they are hard
// links, not a rename.
void FSEventsService::pairCaseRenames(
  const std::vector<Event> &events,
  const std::vector<size_t> &present,
  size_t source,
  std::vector<RenameSide> &sides
) {
  if (mCaseSensitive) {
    return;
  }
  // Else the latest other than the source: the name was changed again since, or can't be read.
  size_t destination = present.back() != source ? present.back() : present.front();
  for (size_t i : present) {
    const std::string &path = events[i].path;
    if (storedName(path) == path.substr(path.find_last_of('/') + 1)) {
      destination = i;
      break;
    }
  }
  for (size_t i : present) {
    if (!equalIgnoringCase(events[i].path, events[destination].path)) {
      return;
    }
  }
  // The other spellings only lead to the entry at the destination.
  for (size_t i : present) {
    sides[i].repeated = i != destination && i != source;
  }
  if (source != destination) {
    pairSides(sides, source, destination);
  }
}

// A place an entry renamed again within the batch passed through: it was never reported there, so only what was
// there before or is there now is news.
void FSEventsService::passThrough(const Event &event, const RenameSide &side) {
  uint64_t reported = 0;
  const bool known = lookUpReported(event.path, reported);
  if (side.exists && !side.replaced && !(known && reported == side.inode)) {
    appear(event.path, side);
  } else if (!side.exists && (known || (event.flags & kFSEventStreamEventFlagItemRemoved) != 0)) {
    remove(event.path, kindsOf(event.flags));
  }
}

// A renamed side with no other side in the batch: an entry that moved in from outside the watched path, as when
// restored from the Trash, one that left and came back within the batch, or one the watcher already reported there and
// whose flags still hold an earlier rename.
void FSEventsService::arrive(const Event &event, const RenameSide &side) {
  if (!mCaseSensitive && !side.returned) {
    std::string directory, name;
    splitFilePath(directory, name, event.path);
    const std::string spelling = storedName(event.path);
    if (!spelling.empty() && spelling != name && equalIgnoringCase(name, spelling)) {
      const std::string destination = (directory == "/" ? directory : directory + "/") + spelling;
      struct stat file;
      if (lstat(destination.c_str(), &file) == 0 && (uint64_t)file.st_ino == side.inode &&
        file.st_dev == side.device) {
        rename(event.path, destination, side.inode, side.kinds);
        return;
      }
    }
  }
  uint64_t reported = 0;
  const bool known = lookUpReported(event.path, reported);
  if (side.returned || (known && reported == side.inode)) {
    auto pending = mGone.find(event.path);
    if (!known && pending != mGone.end() && pending->second.pendingCreation &&
      (pending->second.inode == 0 || pending->second.inode == side.inode)) {
      appear(event.path, side);
    } else if ((event.flags & kModifiedFlags) != 0) {
      modify(event.path, side.kinds.reported);
    }
    return;
  }
  struct timespec added;
  if (!known && !wasReportedGone(event) && (event.flags & kModifiedFlags) != 0 &&
    readAddedTime(event.path, side.inode, added) && !isSinceStart(added)) {
    modify(event.path, side.kinds.reported);
    return;
  }
  // The entry reported there left, under an event FSEvents coalesced with this one.
  if (known) {
    remove(event.path, { PathFilter::Kind::UNKNOWN, kindsOf(event.flags).reported });
  }
  appear(event.path, side);
}

// Reports what a side's path holds now as created, and what it holds if a directory, as FSEvents only reports the
// directory when one comes from elsewhere.
void FSEventsService::appear(const std::string &path, const RenameSide &side) {
  create(path, side.inode, side.kinds);
  if (side.isDirectory) {
    createContents(path, side.inode, side.device);
  }
}

// A renamed side whose entry is gone with no other side in the batch: an entry that moved out of the watched path, as
// when moved to the Trash, or whose other side FSEvents delivers in a later batch. Where it is now tells which.
void FSEventsService::depart(const Event &event, const RenameSide &side) {
  uint64_t reported = 0;
  const bool known = lookUpReported(event.path, reported);
  if (side.exists && known && reported == side.inode) {
    // Its departure was reported before what took its place.
    if ((event.flags & kModifiedFlags) != 0) {
      modify(event.path, side.kinds.reported);
    }
    return;
  }
  // Its departure was reported with an earlier batch, whose flags this one repeats.
  if (!side.exists && wasReportedGone(event)) {
    return;
  }
  // Created and gone with nothing reported there, as classify() reports it.
  if ((event.flags & kFSEventStreamEventFlagItemCreated) != 0 && !known) {
    rememberMissing(event.path, event.inode, true);
  }
  std::string destination;
  // fsgetpath() only looks on the watched path's volume.
  const bool onVolume = side.device == mDevice;
  if (
    event.inode != 0 && onVolume && (event.flags & kFSEventStreamEventFlagItemIsHardlink) == 0 &&
    locate(event.inode, destination) && destination != event.path &&
    !isExcluded(destination)
  ) {
    rename(event.path, destination, event.inode,
      { PathFilter::Kind::UNKNOWN, reportedKindAt(event.flags, destination) });
  } else {
    remove(event.path, kindsOf(event.flags), event.inode);
  }
  if (side.exists && !side.replaced) {
    appear(event.path, side);
  }
}

// Where the entry with inode is now, if its absolute path is inside the watched tree.
bool FSEventsService::locate(uint64_t inode, std::string &path) {
  if (!mCanLocate) {
    return false;
  }
  char buffer[PATH_MAX];
  fsid_t volume = mVolume;
  const ssize_t length = fsgetpath(buffer, sizeof(buffer), &volume, inode);
  if (length <= 0) {
    return false;
  }
  const std::string found(buffer, strnlen(buffer, (size_t)length));
  const std::string prefix = mVolumePath == "/" ? mVolumePath : mVolumePath + "/";
  if (found.size() <= prefix.size() || found.compare(0, prefix.size(), prefix) != 0) {
    return false;
  }
  path = (mPath == "/" ? mPath : mPath + "/") + found.substr(prefix.size());
  if (isOutsideChildren(path.c_str())) {
    return false;
  }
  // Another volume mounted below the watched path can hide the one found there. With another hard link, the path
  // found may be that link rather than where the entry went; a directory's link count only counts what it holds.
  struct stat file;
  return lstat(path.c_str(), &file) == 0 && file.st_dev == mDevice && (uint64_t)file.st_ino == inode &&
    (S_ISDIR(file.st_mode) || file.st_nlink == 1);
}

// A case-insensitive lookup can resolve a spelling that is no longer stored after a rename.
std::string FSEventsService::storedName(const std::string &path) {
  struct attrlist request;
  memset(&request, 0, sizeof(request));
  request.bitmapcount = ATTR_BIT_MAP_COUNT;
  request.commonattr = ATTR_CMN_NAME;
  alignas(4) char buffer[sizeof(uint32_t) + sizeof(attrreference_t) + NAME_MAX * 3 + 1];
  if (getattrlist(path.c_str(), &request, buffer, sizeof(buffer), FSOPT_NOFOLLOW) != 0) {
    return {};
  }
  attrreference_t reference;
  memcpy(&reference, buffer + sizeof(uint32_t), sizeof(reference));
  const size_t offset = sizeof(uint32_t) + (size_t)reference.attr_dataoffset;
  if (reference.attr_dataoffset < 0 || offset > sizeof(buffer) || reference.attr_length > sizeof(buffer) - offset) {
    return {};
  }
  const char *stored = buffer + offset;
  return std::string(stored, strnlen(stored, reference.attr_length));
}

bool FSEventsService::isExcluded(std::string filePath) {
  std::string directory, name;
  splitFilePath(directory, name, filePath);
  return isExcludedDirectory(directory);
}

// Whether what directory holds is excluded.
bool FSEventsService::isExcludedDirectory(const std::string &directory) {
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
  if (event.inode != 0 && event.inode != (uint64_t)file.st_ino) {
    return false;
  }
  if (isSinceStart(file.st_birthtimespec)) {
    return true;
  }
  if ((event.flags & kCreatedFlags) == 0) {
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
void FSEventsService::classify(const Event &event, Kinds kinds) {
  const bool isCreated = (event.flags & kCreatedFlags) != 0;
  const bool isRemoved = (event.flags & kFSEventStreamEventFlagItemRemoved) != 0;
  const bool isModified = (event.flags & kModifiedFlags) != 0;
  if (isModified && !isCreated && !isRemoved) {
    modify(event.path, reportedKindAt(event.flags, event.path));
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
    if (hasCreation && !known) {
      rememberMissing(event.path, event.inode, true);
    }
    remove(event.path, kinds, known ? reported : event.inode);
    return;
  }
  if (known && reported == (uint64_t)file.st_ino) {
    if (isModified || (!isCreated && !isRemoved)) {
      modify(event.path, reportedFromMode(file.st_mode));
    }
    return;
  }
  if (!known && !appearedWhileWatching(event, file)) {
    if (isModified || isRemoved || (event.flags & kFSEventStreamEventFlagItemCreated) != 0) {
      modify(event.path, reportedFromMode(file.st_mode));
    }
    return;
  }
  // The entry reported there was replaced: its flags mix with those of what replaced it, so they only tell its kind
  // when both were of one.
  if (known && isRemoved) {
    remove(event.path, { PathFilter::Kind::UNKNOWN, kinds.reported });
  }
  create(event.path, file.st_ino, { PathFilter::Kind::UNKNOWN, reportedFromMode(file.st_mode) });
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

void FSEventsService::create(const std::string &path, uint64_t inode, Kinds kinds) {
  dispatch(CREATED, path, kinds);
  noteReported(path, inode);
}

void FSEventsService::dispatch(EventType action, std::string path, Kinds kinds) {
  std::string directory, name;

  splitFilePath(directory, name, path);

  mQueue->enqueue(action, directory, name, "", "", kinds.filter, kinds.reported);
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

void FSEventsService::modify(std::string path, PathFilter::Kind kind) {
  dispatch(MODIFIED, path, { PathFilter::Kind::UNKNOWN, kind });
}

void FSEventsService::remove(std::string path, Kinds kinds, uint64_t inode) {
  reportPendingCreation(path, inode, kinds);
  dispatch(DELETED, path, kinds);
  noteGone(path, inode);
}

void FSEventsService::reportPendingCreation(const std::string &path, uint64_t inode, Kinds kinds) {
  const auto found = mGone.find(path);
  if (found != mGone.end() && found->second.pendingCreation && mReported.count(path) == 0 &&
    (found->second.inode == 0 || inode == 0 || found->second.inode == inode)) {
    forgetGone(path);
    dispatch(CREATED, path, kinds);
  }
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

void FSEventsService::rename(const std::string &from, const std::string &to, uint64_t inode, Kinds kinds) {
  reportPendingCreation(from, inode, kinds);
  std::string fromDirectory, fromName, toDirectory, toName;
  splitFilePath(fromDirectory, fromName, from);
  splitFilePath(toDirectory, toName, to);
  const bool hiddenSource = mFilter && mFilter->isDirectoryIgnored(fromDirectory, fromName);
  mQueue->enqueue(RENAMED, fromDirectory, fromName, toDirectory, toName, kinds.filter, kinds.reported);
  // What a directory holds moves along with it, over what the destination held.
  noteReported(to, 0);
  const std::vector<std::pair<std::string, uint64_t>> held(
    mReported.lower_bound(from + "/"), mReported.lower_bound(from + "0"));
  noteGone(from, inode);
  if (!hiddenSource) {
    for (const auto &entry : held) {
      noteReported(to + entry.first.substr(from.size()), entry.second);
    }
  }
  noteReported(to, inode);
  if (mRecursive && hiddenSource) {
    struct stat file;
    if (lstat(to.c_str(), &file) == 0 && (uint64_t)file.st_ino == inode && S_ISDIR(file.st_mode)) {
      createContents(to, inode, file.st_dev);
    }
  }
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
