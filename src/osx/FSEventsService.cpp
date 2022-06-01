#include "../../includes/osx/FSEventsService.h"
#include <iostream>

FSEventsService::FSEventsService(std::shared_ptr<EventQueue> queue, std::string path, const std::vector<std::string> &excludedPaths):
  mPath(path), mQueue(queue), mRootChanged(false) {
  mCaseSensitive = pathconf(path.c_str(), _PC_CASE_SENSITIVE) > 0;
  for (const std::string &excludedPath : excludedPaths) {
    CFStringRef str = CFStringCreateWithCString(
      NULL,
      excludedPath.c_str(),
      kCFStringEncodingUTF8
    );
    mExcludedPaths.push_back(str);
  }

  mRunLoop = new RunLoop(this, path);

  if (!mRunLoop->isLooping()) {
    delete mRunLoop;
    mRunLoop = NULL;
    return;
  }
}

FSEventsService::~FSEventsService() {
  if (mRunLoop != NULL) {
    delete mRunLoop;
  }
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
  char **paths = (char **)eventPaths;
  std::vector<std::string> renamedPaths;
  for (size_t i = 0; i < numEvents && !eventsService->mRootChanged; ++i) {
    if (eventIds[i] == 0 && (eventFlags[i] & kFSEventStreamEventFlagRootChanged) == kFSEventStreamEventFlagRootChanged) {
      eventsService->mRootChanged = true;
      break;
    }
    bool isCreated = (eventFlags[i] & kFSEventStreamEventFlagItemCreated) == kFSEventStreamEventFlagItemCreated;
    bool isRemoved = (eventFlags[i] & kFSEventStreamEventFlagItemRemoved) == kFSEventStreamEventFlagItemRemoved;
    bool isModified = (eventFlags[i] & kFSEventStreamEventFlagItemModified) == kFSEventStreamEventFlagItemModified ||
                      (eventFlags[i] & kFSEventStreamEventFlagItemInodeMetaMod) == kFSEventStreamEventFlagItemInodeMetaMod ||
                      (eventFlags[i] & kFSEventStreamEventFlagItemFinderInfoMod) == kFSEventStreamEventFlagItemFinderInfoMod ||
                      (eventFlags[i] & kFSEventStreamEventFlagItemChangeOwner) == kFSEventStreamEventFlagItemChangeOwner ||
                      (eventFlags[i] & kFSEventStreamEventFlagItemXattrMod) == kFSEventStreamEventFlagItemXattrMod;
    bool isRenamed = (eventFlags[i] & kFSEventStreamEventFlagItemRenamed) == kFSEventStreamEventFlagItemRenamed;

    if(!eventsService->isExcluded(paths[i])) {
      if (isCreated && !(isRemoved || isModified || isRenamed)) {
        eventsService->create(paths[i]);
      } else if (isRemoved && !(isCreated || isModified || isRenamed)) {
        eventsService->remove(paths[i]);
      } else if (isModified && !(isCreated || isRemoved || isRenamed)) {
        eventsService->modify(paths[i]);
      } else if (isRenamed && !(isCreated || isModified || isRemoved)) {
        renamedPaths.push_back(paths[i]);
      } else {
        eventsService->demangle(paths[i]);
      }
    }
  }
  if(!eventsService->mRootChanged) {
    eventsService->rename(renamedPaths);
  }
}

bool FSEventsService::isExcluded(std::string filePath) {
  uint32_t location = filePath.find_last_of("/");
  std::string directory = (location == 0) ? "" : filePath.substr(0, location);
  CFStringRef fileDirectory = CFStringCreateWithCString(
    NULL,
    directory.c_str(),
    kCFStringEncodingUTF8
  );
  CFIndex lenDirectory = CFStringGetLength(fileDirectory);
  CFStringCompareFlags flags;
  if (!mCaseSensitive) {
    flags = kCFCompareLocalized | kCFCompareCaseInsensitive;
  } else {
    flags = kCFCompareLocalized;
  }
  for (CFStringRef excludedPath : mExcludedPaths) {
    CFIndex lenExclude = CFStringGetLength(excludedPath);
    if (lenDirectory >= lenExclude) {
      if (CFStringCompareWithOptions(fileDirectory, excludedPath, CFRangeMake(0, lenExclude), flags) == kCFCompareEqualTo) {
        return true;
      }
    }
  }
  return false;
}

void FSEventsService::create(std::string path) {
  dispatch(CREATED, path);
}

void FSEventsService::demangle(std::string path) {
  struct stat file;
  if (stat(path.c_str(), &file) != 0) {
    remove(path);
    return;
  }

  if (file.st_birthtimespec.tv_sec != file.st_mtimespec.tv_sec) {
    modify(path);
  } else {
    create(path);
  }
}

void FSEventsService::dispatch(EventType action, std::string path) {
  std::string directory, name;

  splitFilePath(directory, name, path);

  mQueue->enqueue(action, directory, name);
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

bool FSEventsService::isWatching() {
  return mRunLoop != NULL && mRunLoop->isLooping();
}

void FSEventsService::modify(std::string path) {
  dispatch(MODIFIED, path);
}

void FSEventsService::remove(std::string path) {
  dispatch(DELETED, path);
}

void FSEventsService::rename(std::vector<std::string> &paths) {
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
        demangle(fullSideA);
        demangle(fullSideB);
      }

    } else {
      for (auto pathIterator = binIterator->second->begin(); pathIterator != binIterator->second->end(); ++pathIterator) {
        demangle(binIterator->first + "/" + *pathIterator);
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
  mExcludedPaths.clear();
  for (const std::string &path : excludedPaths) {
    CFStringRef pathRef = CFStringCreateWithCString(
      NULL,
      path.c_str(),
      kCFStringEncodingUTF8
    );
    mExcludedPaths.push_back(pathRef);
  }
}
