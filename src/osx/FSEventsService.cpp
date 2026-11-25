#include "../../includes/osx/FSEventsService.h"
#include <cstring>
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
  eventsService->handle(events);
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

  // What a removed entry was, which the filter can't look up anymore. Not when the flags coalesce two entries. A
  // symlink counts as a directory, which it most often leads to, as the filter follows it when it can, so its
  // removal is judged like its creation was.
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

void FSEventsService::handle(const std::vector<Event> &events) {
  std::vector<std::string> renamedPaths;
  std::map<std::string, PathFilter::Kind> renamedKinds;
  for (const Event &event : events) {
    const bool isRenamed = (event.flags & kFSEventStreamEventFlagItemRenamed) != 0;
    const bool isOther = (event.flags & (kFSEventStreamEventFlagItemCreated | kFSEventStreamEventFlagItemRemoved |
      kModifiedFlags)) != 0;
    if (isRenamed && !isOther) {
      renamedPaths.push_back(event.path);
      renamedKinds[event.path] = kindOf(event.flags);
    } else {
      classify(event, kindOf(event.flags));
    }
  }
  if (!mRootChanged) {
    rename(renamedPaths, renamedKinds);
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

void FSEventsService::demangle(std::string path, PathFilter::Kind kind) {
  classify(Event{path, 0, 0, 0}, kind);
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

// kinds: what each path was, for one that is gone.
void FSEventsService::rename(std::vector<std::string> &paths, const std::map<std::string, PathFilter::Kind> &kinds) {
  auto kindOf = [&kinds](const std::string &path) {
    auto found = kinds.find(path);
    return found == kinds.end() ? PathFilter::Kind::UNKNOWN : found->second;
  };
  auto *binNamesByPath = new std::map<std::string, std::vector<std::string> *>;

  for (const std::string &path : paths) {
    std::string directory, name;
    splitFilePath(directory, name, path);
    if (binNamesByPath->find(directory) == binNamesByPath->end()) {
      (*binNamesByPath)[directory] = new std::vector<std::string>;
    }
    (*binNamesByPath)[directory]->push_back(name);
  }

  for (auto binIterator = binNamesByPath->begin(); binIterator != binNamesByPath->end(); ++binIterator) {
    if (binIterator->second->size() == 2) {
      std::string sideA = (*binIterator->second)[0],
                  sideB = (*binIterator->second)[1];
      std::string fullSideA = binIterator->first + "/" + sideA,
                  fullSideB = binIterator->first + "/" + sideB;
      struct stat renameSideA, renameSideB;
      bool sideAExists = stat(fullSideA.c_str(), &renameSideA) == 0,
           sideBExists = stat(fullSideB.c_str(), &renameSideB) == 0;

      if (sideAExists && !sideBExists) {
        mQueue->enqueue(RENAMED, binIterator->first, sideB, binIterator->first, sideA);
        noteReported(fullSideB, 0);
        noteReported(fullSideA, renameSideA.st_ino);
      } else if (!sideAExists && sideBExists) {
        mQueue->enqueue(RENAMED, binIterator->first, sideA, binIterator->first, sideB);
        noteReported(fullSideA, 0);
        noteReported(fullSideB, renameSideB.st_ino);
      } else {
        demangle(fullSideA, kindOf(fullSideA));
        demangle(fullSideB, kindOf(fullSideB));
      }

    } else {
      for (auto pathIterator = binIterator->second->begin(); pathIterator != binIterator->second->end(); ++pathIterator) {
        const std::string path = binIterator->first + "/" + *pathIterator;
        demangle(path, kindOf(path));
      }
    }
    delete binIterator->second;
    binIterator->second = NULL;
  }
  delete binNamesByPath;
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
