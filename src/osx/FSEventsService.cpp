#include "../../includes/osx/FSEventsService.h"
#include <cstring>
#include <iostream>

FSEventsService::FSEventsService(std::shared_ptr<EventQueue> queue, std::string path, const std::vector<std::string> &excludedPaths,
  bool recursive):
  mPath(path), mQueue(queue), mRootChanged(false), mRecursive(recursive) {
  mCaseSensitive = pathconf(path.c_str(), _PC_CASE_SENSITIVE) > 0;
  mExcludedPaths = createExcludedPaths(excludedPaths);
  mRootExcluded = excludesRoot(mExcludedPaths);

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
  const CFStringCompareFlags flags = mCaseSensitive ? kCFCompareLocalized : kCFCompareLocalized | kCFCompareCaseInsensitive;
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
    if (eventIds[i] == 0 && (eventFlags[i] & kFSEventStreamEventFlagRootChanged) == kFSEventStreamEventFlagRootChanged) {
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
    if (eventsService->isExcluded(event.path)) {
      continue;
    }
    event.flags = eventFlags[i];
    event.id = eventIds[i];
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
    const FSEventStreamEventFlags flags = event.flags;
    bool isCreated = (flags & kFSEventStreamEventFlagItemCreated) == kFSEventStreamEventFlagItemCreated;
    bool isRemoved = (flags & kFSEventStreamEventFlagItemRemoved) == kFSEventStreamEventFlagItemRemoved;
    bool isModified = (flags & kFSEventStreamEventFlagItemModified) == kFSEventStreamEventFlagItemModified ||
                      (flags & kFSEventStreamEventFlagItemInodeMetaMod) == kFSEventStreamEventFlagItemInodeMetaMod ||
                      (flags & kFSEventStreamEventFlagItemFinderInfoMod) == kFSEventStreamEventFlagItemFinderInfoMod ||
                      (flags & kFSEventStreamEventFlagItemChangeOwner) == kFSEventStreamEventFlagItemChangeOwner ||
                      (flags & kFSEventStreamEventFlagItemXattrMod) == kFSEventStreamEventFlagItemXattrMod;
    bool isRenamed = (flags & kFSEventStreamEventFlagItemRenamed) == kFSEventStreamEventFlagItemRenamed;
    const PathFilter::Kind kind = kindOf(flags);
    if (isCreated && !(isRemoved || isModified || isRenamed)) {
      create(event.path);
    } else if (isRemoved && !(isCreated || isModified || isRenamed)) {
      remove(event.path, kind);
    } else if (isModified && !(isCreated || isRemoved || isRenamed)) {
      modify(event.path);
    } else if (isRenamed && !(isCreated || isModified || isRemoved)) {
      renamedPaths.push_back(event.path);
      renamedKinds[event.path] = kind;
    } else {
      demangle(event.path, kind);
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
  CFStringCompareFlags flags;
  if (!mCaseSensitive) {
    flags = kCFCompareLocalized | kCFCompareCaseInsensitive;
  } else {
    flags = kCFCompareLocalized;
  }
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

void FSEventsService::create(std::string path) {
  dispatch(CREATED, path);
}

void FSEventsService::demangle(std::string path, PathFilter::Kind kind) {
  struct stat file;
  if (stat(path.c_str(), &file) != 0) {
    remove(path, kind);
    return;
  }

  if (file.st_birthtimespec.tv_sec != file.st_mtimespec.tv_sec) {
    modify(path);
  } else {
    create(path);
  }
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

void FSEventsService::remove(std::string path, PathFilter::Kind kind) {
  dispatch(DELETED, path, kind);
}

// kinds: what each path was, for one that is gone, see demangle().
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
      } else if (!sideAExists && sideBExists) {
        mQueue->enqueue(RENAMED, binIterator->first, sideA, binIterator->first, sideB);
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
