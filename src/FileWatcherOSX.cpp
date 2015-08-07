#include "../includes/FileWatcherOSX.h"
#include <iostream>
namespace NSFW {

  FileWatcherOSX::FileWatcherOSX(std::string path, std::queue<Event> &eventsQueue, bool &watchFiles)
    : mEventsQueue(eventsQueue), mPath(path), mWatchFiles(watchFiles) {}

  std::string FileWatcherOSX::getPath() {
    return mPath;
  }

  bool FileWatcherOSX::start() {
    if (pthread_create(&mThread, 0, &FileWatcherOSX::mainLoop, (void *)this)) {
      return true;
    } else {
      return false;
    }
  }

  void FileWatcherOSX::snapshotDir() {
    std::queue<Directory *> dirQueue;
    struct dirent ** namelist = NULL;
    int n = scandir(mPath.c_str(), &namelist, NULL, alphasort);

    if (n < 0) {
      // kill mainloop
      return;
    }

    // create root of snapshot
    struct Directory *root = new Directory;
    root->entry = NULL;
    root->entries = namelist;
    root->numEntries = n;
    root->path = "";
    root->numChildren = 0;

    // find all the directories within this directory
    std::queue<int> childLocation;
    for (int i = 0; i < n; ++i) {
      if (root->entries[i]->d_type == DT_DIR
        && strcmp(root->entries[i]->d_name, ".")
        && strcmp(root->entries[i]->d_name, "..")) {
        root->numChildren++;
        childLocation.push(i);
      }
    }

    // create the container array for childDirectories
    root->childDirectories = new Directory[root->numChildren];

    // enqueue directories for snapshot
    while (!childLocation.empty()) {
      Directory *dir = &(root->childDirectories[childLocation.size() - 1]);
      dir->entry = root->entries[childLocation.front()];
      childLocation.pop();
      dir->path = root->path + "/" + dir->entry->d_name;
      dirQueue.push(dir);
    }

  }

  void FileWatcherOSX::callback(
      ConstFSEventStreamRef streamRef,
      void *clientCallBackInfo,
      size_t numEvents,
      void *eventPaths,
      const FSEventStreamEventFlags eventFlags[],
      const FSEventStreamEventId eventIds[])
  {
      int i;
      FileWatcherOSX *fwOSX = (FileWatcherOSX *)clientCallBackInfo;
  }

  void *FileWatcherOSX::mainLoop(void *params) {
    FileWatcherOSX *fwOSX = (FileWatcherOSX *)params;
    CFStringRef mypath = CFStringCreateWithCString(
      NULL,
      (char *)(fwOSX->getPath().c_str()),
      kCFStringEncodingUTF8);
    CFArrayRef pathsToWatch = CFArrayCreate(NULL, (const void **)&mypath, 1, NULL);
    FSEventStreamRef stream;
    CFAbsoluteTime latency = 1.0;

    FSEventStreamContext callbackInfo;
    callbackInfo.version = 0;
    callbackInfo.info = params;

    /* Create the stream, passing in a callback */
    stream = FSEventStreamCreate(NULL,
        &FileWatcherOSX::callback,
        &callbackInfo,
        pathsToWatch,
        kFSEventStreamEventIdSinceNow, /* Or a previous event ID */
        latency,
        kFSEventStreamCreateFlagNone /* Flags explained in reference */
    );



    FSEventStreamScheduleWithRunLoop(stream, CFRunLoopGetCurrent(), kCFRunLoopDefaultMode);
    FSEventStreamStart(stream);
    CFRunLoopRun();
  }

}
