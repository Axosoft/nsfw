#include "../../includes/osx/FSEventsService.h"

#define MS_TO_NS 1000000

FSEventsService::FSEventsService(std::string path):
  mWatching(false) {
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
  std::vector<std::string> *renamedPaths = new std::vector<std::string>;
  for (size_t i = 0; i < numEvents; ++i) {
    bool isCreated = (eventFlags[i] & kFSEventStreamEventFlagItemCreated) == kFSEventStreamEventFlagItemCreated;
    bool isRemoved = (eventFlags[i] & kFSEventStreamEventFlagItemRemoved) == kFSEventStreamEventFlagItemRemoved;
    bool isModified = (eventFlags[i] & kFSEventStreamEventFlagItemModified) == kFSEventStreamEventFlagItemModified ||
                      (eventFlags[i] & kFSEventStreamEventFlagItemInodeMetaMod) == kFSEventStreamEventFlagItemInodeMetaMod ||
                      (eventFlags[i] & kFSEventStreamEventFlagItemFinderInfoMod) == kFSEventStreamEventFlagItemFinderInfoMod ||
                      (eventFlags[i] & kFSEventStreamEventFlagItemChangeOwner) == kFSEventStreamEventFlagItemChangeOwner ||
                      (eventFlags[i] & kFSEventStreamEventFlagItemXattrMod) == kFSEventStreamEventFlagItemXattrMod;
    bool isRenamed = (eventFlags[i] & kFSEventStreamEventFlagItemRenamed) == kFSEventStreamEventFlagItemRenamed;

    if (isCreated && !(isRemoved || isModified || isRenamed)) {
      eventsService->create(paths[i]);
    } else if (isRemoved && !(isCreated || isModified || isRenamed)) {
      eventsService->remove(paths[i]);
    } else if (isModified && !(isCreated || isRemoved || isRenamed)) {
      eventsService->modify(paths[i]);
    } else if (isRenamed && !(isCreated || isModified || isRemoved)) {
      renamedPaths->push_back(paths[i]);
    } else {
      eventsService->demangle(paths[i]);
    }
  }
  eventsService->rename(renamedPaths);
  delete renamedPaths;
}

void FSEventsService::create(std::string path) {
  std::string directory, name;

  splitFilePath(directory, name, path);

  std::cout
    << "CREATE: "
    << name
    << " in "
    << directory
    << std::endl;
}

void FSEventsService::demangle(std::string path) {
  struct stat file;
  if (stat(path.c_str(), &file) != 0) {
    remove(path);
    return;
  }

  if (file.st_birthtimespec.tv_sec != time(NULL)) {
    modify(path);
  } else {
    create(path);
  }
}

void FSEventsService::modify(std::string path) {
  std::string directory, name;

  splitFilePath(directory, name, path);

  std::cout
    << "MODIFY: "
    << name
    << " in "
    << directory
    << std::endl;
}

void FSEventsService::remove(std::string path) {
  std::string directory, name;

  splitFilePath(directory, name, path);

  std::cout
    << "REMOVE: "
    << name
    << " in "
    << directory
    << std::endl;
}

void FSEventsService::rename(std::vector<std::string> *paths) {
  auto *binNamesByPath = new std::map<std::string, std::vector<std::string> *>;

  for (auto pathIterator = paths->begin(); pathIterator != paths->end(); ++pathIterator) {
    std::string directory, name;
    splitFilePath(directory, name, *pathIterator);
    if (binNamesByPath->find(directory) == binNamesByPath->end()) {
      (*binNamesByPath)[directory] = new std::vector<std::string>;
    }
    (*binNamesByPath)[directory]->push_back(name);
  }

  for (auto binIterator = binNamesByPath->begin(); binIterator != binNamesByPath->end(); ++binIterator) {
    if (binIterator->second->size() == 2) {
      std::string sideA = (*binIterator->second)[0],
                  sideB = (*binIterator->second)[1];
      struct stat renameSideA, renameSideB;
      bool sideAExists = stat((binIterator->first + "/" + sideA).c_str(), &renameSideA) == 0,
           sideBExists = stat((binIterator->first + "/" + sideB).c_str(), &renameSideB) == 0;

      if (sideAExists != sideBExists) {
        std::cout
          << "RENAMED ["
          << (sideAExists ? sideB : sideA)
          << "] to ["
          << (sideAExists ? sideA : sideB)
          << "] in "
          << binIterator->first
          << std::endl;
      } else {
        demangle(sideA);
        demangle(sideB);
      }

    } else {
      for (auto pathIterator = binIterator->second->begin(); pathIterator != binIterator->second->end(); ++pathIterator) {
        demangle(*pathIterator);
      }
    }
    delete binIterator->second;
    binIterator->second = NULL;
  }
  delete binNamesByPath;
}

void FSEventsService::splitFilePath(std::string &directory, std::string &name, std::string path) {
  if (path.length() == 1 && path[0] == '/') {
    directory = "/";
    name = "";
  } else {
    uint32_t location = path.find_last_of("/");
    directory = (location == 0) ? "/" : path.substr(0, location);
    name = path.substr(location + 1);
  }
}
