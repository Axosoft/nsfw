#include "../includes/FileWatcherOSX.h"
#include <iostream>
namespace NSFW {

  FileWatcherOSX::FileWatcherOSX(std::string path, std::queue<Event> &eventsQueue, bool &watchFiles)
    : mEventsQueue(eventsQueue), mPath(path), mWatchFiles(watchFiles) {}

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
        fwOSX->processCallback();
    }

  std::string FileWatcherOSX::getPath() {
    return mPath;
  }

  void *FileWatcherOSX::mainLoop(void *params) {
    // load initial dir tree
    ((FileWatcherOSX *)params)->mDirTree = ((FileWatcherOSX *)params)->snapshotDir();

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

  void FileWatcherOSX::processCallback() {
    Directory *currentTree = snapshotDir(), *pastSnapshotPtr = mDirTree;
    Directory *currentSnapshotPtr = currentTree;

    if (currentTree == NULL) {
      // handle errors
      return;
    }


  }

  Directory *FileWatcherOSX::snapshotDir() {
    std::queue<Directory *> dirQueue;
    Directory *topRoot = new Directory;

    // create root of snapshot
    topRoot->entry = NULL;
    topRoot->path = mPath;
    topRoot->numChildren = 0;

    dirQueue.push(topRoot);

    while (!dirQueue.empty()) {
      Directory *root = dirQueue.front();
      dirent ** directoryContents = NULL;
      int n = scandir(root->path.c_str(), &directoryContents, NULL, alphasort);

      if (n < 0) {
        return NULL; // add error handling
      }

      root->numChildren = 0;

      // find all the directories within this directory
      // this breaks the alphabetical sorting of directories
      std::queue<int> childLocation;
      for (int i = 0; i < n; ++i) {
        if (!strcmp(directoryContents[i]->d_name, ".") || !strcmp(directoryContents[i]->d_name, ".."))
          continue; // skip navigation folder

        if (directoryContents[i]->d_type == DT_DIR)
        {
          root->numChildren++;
          childLocation.push(i);
        } else {
          ino_t inode = directoryContents[i]->d_ino;
          FileDescriptor fd;
          fd.entry = directoryContents[i];
          fd.path = root->path + "/" + fd.entry->d_name;
          int error = stat(fd.path.c_str(), &fd.meta);

          if (error < 0) {
            return NULL; // add error handling
          }

          root->fileMap[inode] = fd;
        }
      }

      // create the container array for childDirectories
      root->childDirectories = new Directory[root->numChildren];

      // enqueue directories for snapshot
      while (!childLocation.empty()) {
        Directory *dir = &(root->childDirectories[childLocation.size() - 1]);
        dir->entry = directoryContents[childLocation.front()];
        childLocation.pop();
        dir->path = root->path + "/" + dir->entry->d_name;
        dirQueue.push(dir);
      }

      delete[] directoryContents;
      dirQueue.pop();
    }

    return topRoot;
  }

  bool FileWatcherOSX::start() {
    if (pthread_create(&mThread, 0, &FileWatcherOSX::mainLoop, (void *)this)) {
      return true;
    } else {
      return false;
    }
  }
}
