#include "../includes/FileWatcherOSX.h"
#include <iostream>

namespace NSFW {

  FileWatcherOSX::FileWatcherOSX(std::string path, std::queue<Event> &eventsQueue, bool &watchFiles, Error &error)
    : mDie(false), mDirTree(NULL), mError(error), mEventsQueue(eventsQueue), mIsDirWatch(false), mPath(path), mWatchFiles(watchFiles)
  {
    pthread_mutexattr_t attr;
    if (pthread_mutexattr_init(&attr) == 0)
    {
      pthread_mutex_init(&mCallbackSync, &attr);
      pthread_mutex_init(&mMainLoopSync, &attr);
    }
  }

  FileWatcherOSX::~FileWatcherOSX()
  {
    pthread_mutex_destroy(&mCallbackSync);
    pthread_mutex_destroy(&mMainLoopSync);
  }

  void FileWatcherOSX::callback(
      ConstFSEventStreamRef streamRef,
      void *clientCallBackInfo,
      size_t numEvents,
      void *eventPaths,
      const FSEventStreamEventFlags eventFlags[],
      const FSEventStreamEventId eventIds[])
  {
      FileWatcherOSX *fwOSX = (FileWatcherOSX *)clientCallBackInfo;
      fwOSX->processDirCallback();
  }

  bool FileWatcherOSX::checkTimeValEquality(struct timespec *x, struct timespec *y)
  {
    return x->tv_sec == y->tv_sec && x->tv_nsec == y->tv_nsec;
  }

  void FileWatcherOSX::deleteDirTree(Directory *tree)
  {
    if (tree == NULL)
    {
      return;
    }

    std::queue<Directory *> dirQueue;

    dirQueue.push(tree);

    while (!dirQueue.empty())
    {
      Directory *root = dirQueue.front();

      // Add directories to the queue to continue deleting directories/files
      for (std::map<ino_t, Directory *>::iterator dirIter = root->childDirectories.begin();
        dirIter != root->childDirectories.end(); ++dirIter)
      {
        dirQueue.push(dirIter->second);
      }

      dirQueue.pop();

      delete root;
    }
  }

  void FileWatcherOSX::filePoller()
  {
    std::string name, path;

    size_t lastSlash = mPath.find_last_of("/");
    if (lastSlash != std::string::npos)
    {
      name = mPath.substr(lastSlash + 1);
      path = mPath.substr(0, lastSlash);
    } else {
      name = mPath;
      path = "/";
    }

    while (mWatchFiles)
    {
      FilePoll snapshot;
      int error = stat(mPath.c_str(), &snapshot.file);
      if (error < 0)
      {
        if (mFile.exists)
        {
          Event event;
          event.directory = path;
          event.file[0] = name;
          event.action = DELETED;
          mEventsQueue.push(event);
          mFile.exists = false;
          usleep(1000);
          continue;
        }
        else
        {
          usleep(1000);
          continue;
        }
      }

      snapshot.exists = true;

      if (mFile.exists && !checkTimeValEquality(&mFile.file.st_birthtimespec, &snapshot.file.st_birthtimespec))
      {
        Event eventA, eventB;
        eventA.directory = path;
        eventA.file[0] = name;
        eventA.action = DELETED;
        mEventsQueue.push(eventA);

        eventB.directory = path;
        eventB.file[0] = name;
        eventB.action = CREATED;
        mEventsQueue.push(eventB);
        mFile = snapshot;
      }

      if (!mFile.exists)
      {
        Event event;
        event.directory = path;
        event.file[0] = name;
        event.action = CREATED;
        mEventsQueue.push(event);
        mFile = snapshot;
      }
      else if (!checkTimeValEquality(&mFile.file.st_mtimespec, &snapshot.file.st_mtimespec)
        || !checkTimeValEquality(&mFile.file.st_ctimespec, &snapshot.file.st_ctimespec))
      {
        Event event;
        event.directory = path;
        event.file[0] = name;
        event.action = MODIFIED;
        mEventsQueue.push(event);
        mFile = snapshot;
      }
    }
    usleep(1000);
  }

  std::string FileWatcherOSX::getPath()
  {
    return mPath;
  }

  // traverses the tree under a directory and adds every item as an event of type action
  void FileWatcherOSX::handleTraversingDirectoryChange(Action action, Directory *directory)
  {
    std::queue<Directory *> dirQueue;

    dirQueue.push(directory);

    while (!dirQueue.empty())
    {
      Directory *root = dirQueue.front();
      // Events for all files in this 'root' directory
      for (std::map<ino_t, FileDescriptor>::iterator fileIter = root->fileMap.begin();
        fileIter != root->fileMap.end(); ++fileIter)
      {
        Event event;
        event.directory = root->path + "/" + root->name;
        event.file[0] = fileIter->second.name;
        event.action = action;
        mEventsQueue.push(event);
      }

      // Add directories to the queue to continue listing events
      for (std::map<ino_t, Directory *>::iterator dirIter = root->childDirectories.begin();
        dirIter != root->childDirectories.end(); ++dirIter)
      {
        dirQueue.push(dirIter->second);
      }

      Event event;
      event.directory = root->path;
      event.file[0] = root->name;
      event.action = action;
      mEventsQueue.push(event);

      dirQueue.pop();
    }
  }

  void *FileWatcherOSX::mainLoop(void *params)
  {
    // load initial dir tree
    FileWatcherOSX *fwOSX = (FileWatcherOSX *)params;

    std::string path = fwOSX->getPath();

    struct stat fileInfo;
    int error = stat(path.c_str(), &fileInfo);

    if (error < 0)
    {
      fwOSX->setErrorMessage("Access is denied");
      return NULL;
    }

    if (S_ISDIR(fileInfo.st_mode))
    {
      fwOSX->mIsDirWatch = true;
      pthread_mutex_lock(&fwOSX->mMainLoopSync);
      fwOSX->mDirTree = fwOSX->snapshotDir();
      CFStringRef mypath = CFStringCreateWithCString(
        NULL,
        (char *)(fwOSX->getPath().c_str()), // the path that the file watcher should watch
        kCFStringEncodingUTF8);
      CFArrayRef pathsToWatch = CFArrayCreate(NULL, (const void **)&mypath, 1, NULL);
      CFAbsoluteTime latency = 0.01;

      FSEventStreamContext callbackInfo;
      callbackInfo.version = 0;
      callbackInfo.info = params; // pass the calling filewatcherosx object into the callback

      /* Create the stream, passing in a callback */
      fwOSX->mStream = FSEventStreamCreate(NULL,
          &FileWatcherOSX::callback,
          &callbackInfo,
          pathsToWatch,
          kFSEventStreamEventIdSinceNow,
          latency,
          kFSEventStreamCreateFlagFileEvents
      );
      CFRelease(pathsToWatch);
      CFRelease(mypath);

      fwOSX->mRunLoop = CFRunLoopGetCurrent();
      CFRetain(fwOSX->mRunLoop);


      FSEventStreamScheduleWithRunLoop(fwOSX->mStream, fwOSX->mRunLoop, kCFRunLoopDefaultMode);
      FSEventStreamStart(fwOSX->mStream);

      CFRunLoopTimerContext ctx;
      ctx.info = fwOSX;

      CFRunLoopTimerRef timer = CFRunLoopTimerCreate(
        NULL, // allocator
        0, // fireDate
        0.01, // interval
        0, // flags
        0, // order
        &FileWatcherOSX::timerCallback,
        &ctx
      );

      CFRunLoopAddTimer(fwOSX->mRunLoop, timer, kCFRunLoopCommonModes);
      CFRelease(timer);

      CFRunLoopRun();

      CFRelease(fwOSX->mRunLoop);

      pthread_mutex_unlock(&fwOSX->mMainLoopSync);
    }
    else if (S_ISREG(fileInfo.st_mode))
    {
      fwOSX->mFile.file = fileInfo;
      fwOSX->mDirTree = NULL;
      fwOSX->mFile.exists = true;
      fwOSX->filePoller();
    }
    else
    {
      fwOSX->setErrorMessage("Access is denied");
      return NULL;
    }

    return NULL;
  }

  void FileWatcherOSX::processDirCallback()
  {
    // only run this process if mWatchFiles is true, and we can get a lock on the mutex
    if (mWatchFiles && pthread_mutex_lock(&mCallbackSync) != 0)
    {
      pthread_mutex_unlock(&mCallbackSync);
      return;
    }

    Directory *currentTree = snapshotDir();
    std::queue<DirectoryPair> dirPairQueue;

    // in case a directory/file was deleted while a scan was active
    if (currentTree == NULL)
    {
      dirent ** directoryContents = NULL;
      int m = scandir(mPath.c_str(), &directoryContents, NULL, alphasort);

      for (int i = 0; i < m; ++i)
      {
        delete directoryContents[i];
      }

      delete directoryContents;

      if (m < 0)
      {
        setErrorMessage("Access is denied");
      }

      pthread_mutex_unlock(&mCallbackSync);
      return;
    }

    DirectoryPair snapshot;
    snapshot.prev = mDirTree;
    snapshot.current = currentTree;

    dirPairQueue.push(snapshot);

    while(!dirPairQueue.empty())
    {
      // get a DirectoryPair
      snapshot = dirPairQueue.front();

      // compare files in the directory ----------------------------------------
      // -----------------------------------------------------------------------

      std::map<ino_t, FileDescriptor> currentFileMapCopy(snapshot.current->fileMap);
      std::string currentPath = snapshot.current->path + "/" + snapshot.current->name;


      // iterate through old snapshot, compare to new snapshot
      for (std::map<ino_t, FileDescriptor>::iterator fileIter = snapshot.prev->fileMap.begin();
        fileIter != snapshot.prev->fileMap.end(); ++fileIter)
      {
        std::map<ino_t, FileDescriptor>::iterator currentComparableFilePtr = currentFileMapCopy.find(fileIter->first);
        // deleted event
        if (currentComparableFilePtr == currentFileMapCopy.end())
        {
          Event event;
          event.directory = currentPath;
          event.file[0] = fileIter->second.name;
          event.action = DELETED;
          mEventsQueue.push(event);
          continue;
        }

        // renamed event
        if (fileIter->second.name != currentComparableFilePtr->second.name)
        {
          Event event;
          event.directory = currentPath;
          event.file[0] = fileIter->second.name;
          event.file[1] = currentComparableFilePtr->second.name;
          event.action = RENAMED;
          mEventsQueue.push(event);
        }

        // changed event
        if (!checkTimeValEquality(&fileIter->second.meta.st_mtimespec, &currentComparableFilePtr->second.meta.st_mtimespec)
         || !checkTimeValEquality(&fileIter->second.meta.st_ctimespec, &currentComparableFilePtr->second.meta.st_ctimespec))
        {
          Event event;
          event.directory = currentPath;
          event.file[0] = fileIter->second.name;
          event.action = MODIFIED;
          mEventsQueue.push(event);
        }

        currentFileMapCopy.erase(currentComparableFilePtr);
      }

      // find all new files
      for (std::map<ino_t, FileDescriptor>::iterator fileIter = currentFileMapCopy.begin();
        fileIter != currentFileMapCopy.end(); ++fileIter)
      {
        // created event
        Event event;
        event.directory = currentPath;
        event.file[0] = fileIter->second.name;
        event.action = CREATED;
        mEventsQueue.push(event);
      }

      // compare directory structure -------------------------------------------
      // -----------------------------------------------------------------------

      std::map<ino_t, Directory *> currentChildDirectories(snapshot.current->childDirectories);

      // iterate through old snapshot, compare to new snapshot
      for(std::map<ino_t, Directory *>::iterator dirIter = snapshot.prev->childDirectories.begin();
        dirIter != snapshot.prev->childDirectories.end(); ++dirIter)
      {
        std::map<ino_t, Directory *>::iterator currentComparableDirPtr = currentChildDirectories.find(dirIter->first);

        // deleted event
        if (currentComparableDirPtr == currentChildDirectories.end())
        {
          // add all associated delete events for this directory deletion
          handleTraversingDirectoryChange(DELETED, dirIter->second);
          continue;
        }

        // renamed event
        if (dirIter->second->name != currentComparableDirPtr->second->name)
        {
          Event event;
          event.directory = currentPath;
          event.file[0] = dirIter->second->name;
          event.file[1] = currentComparableDirPtr->second->name;
          event.action = RENAMED;
          mEventsQueue.push(event);
        }

        // create a new pair for comparison
        DirectoryPair enqueueSnapshot;
        enqueueSnapshot.prev = dirIter->second;
        enqueueSnapshot.current = currentComparableDirPtr->second;

        // push the new pair to the queue
        dirPairQueue.push(enqueueSnapshot);

        // remove the directory so that we can discover added directories faster
        currentChildDirectories.erase(currentComparableDirPtr);
      }

      for (std::map<ino_t, Directory *>::iterator dirIter = currentChildDirectories.begin();
        dirIter != currentChildDirectories.end(); ++dirIter)
      {
        // add all associated created events for this directory deletion
        handleTraversingDirectoryChange(CREATED, dirIter->second);
      }

      // remove the snapshot from the queue
      dirPairQueue.pop();
    }

    // delete mDirTree
    deleteDirTree(mDirTree);

    // assign currentTree to mDirTree
    mDirTree = currentTree;

    pthread_mutex_unlock(&mCallbackSync);
  }

  Directory *FileWatcherOSX::snapshotDir()
  {
    std::queue<Directory *> dirQueue;
    Directory *topRoot = new Directory;

    // create root of snapshot
    size_t lastSlash = mPath.find_last_of("/");
    if (lastSlash != std::string::npos)
    {
      topRoot->path = mPath.substr(0, lastSlash);
      topRoot->name = mPath.substr(lastSlash + 1);
    }
    else
    {
      topRoot->path = "";
      topRoot->name = mPath;
    }

    dirQueue.push(topRoot);

    while (!dirQueue.empty())
    {
      Directory *root = dirQueue.front();
      dirent ** directoryContents = NULL;
      std::string rootPath = root->path + "/" + root->name;

      int n = scandir(rootPath.c_str(), &directoryContents, NULL, alphasort);

      if (n < 0)
      {
        for (int i = 0; i < n; ++i)
        {
          delete directoryContents[i];
        }

        delete directoryContents;

        deleteDirTree(topRoot);
        return NULL;
      }

      // find all the directories within this directory
      // this breaks the alphabetical sorting of directories
      std::queue<int> childLocation;
      bool failure = false;
      for (int i = 0; i < n; ++i)
      {
        if (!strcmp(directoryContents[i]->d_name, ".") || !strcmp(directoryContents[i]->d_name, ".."))
        {
          continue; // skip navigation folder
        }

        ino_t inode = directoryContents[i]->d_ino;

        if (directoryContents[i]->d_type == DT_DIR)
        {
          // create the directory struct for this directory and add a reference of this directory to its root
          Directory *dir = new Directory;
          dir->name = directoryContents[i]->d_name;
          dir->path = rootPath;
          root->childDirectories[inode] = dir;
          dirQueue.push(dir);
        }
        else
        {
          // store the file information in a quick data structure for later
          FileDescriptor fd;
          fd.name = directoryContents[i]->d_name;
          fd.path = rootPath + "/" + fd.name;
          int error = stat(fd.path.c_str(), &fd.meta);

          if (error < 0)
          {
            failure = true;
            break;
          }

          root->fileMap[inode] = fd;
        }
      }

      for (int i = 0; i < n; ++i)
      {
        delete directoryContents[i];
      }

      delete[] directoryContents;

      if (failure)
      {
        deleteDirTree(topRoot);
        return NULL;
      }

      dirQueue.pop();
    }

    return topRoot;
  }

  void FileWatcherOSX::setErrorMessage(std::string message)
  {
    mError.status = true;
    mError.message = message;
  }

  bool FileWatcherOSX::start()
  {
    // test mutex for init
    if (pthread_mutex_lock(&mCallbackSync) != 0 || pthread_mutex_lock(&mMainLoopSync) != 0)
    {
      return false; // if it fails, let caller know that this is not started
    }
    else
    {
      pthread_mutex_unlock(&mCallbackSync);
      pthread_mutex_unlock(&mMainLoopSync);
    }

    if (mWatchFiles && pthread_create(&mThread, 0, &FileWatcherOSX::mainLoop, (void *)this) == 0)
    {
      return true;
    }
    else
    {
      return false;
    }
  }

  void FileWatcherOSX::stop()
  {
    if (mIsDirWatch)
    {
      mDie = true;

      pthread_mutex_lock(&mMainLoopSync);
      // safely kill the thread
      pthread_join(mThread, NULL);
      if (mDirTree != NULL)
      {
        deleteDirTree(mDirTree);
        mDirTree = NULL;
      }

      pthread_mutex_unlock(&mMainLoopSync);
    }
    else
    {
      int t;
      // safely kill the thread
      pthread_setcancelstate(PTHREAD_CANCEL_ASYNCHRONOUS, &t);
      pthread_cancel(mThread);
    }
  }

  void FileWatcherOSX::timerCallback(CFRunLoopTimerRef timer, void* callbackInfo)
  {
    FileWatcherOSX* fwOSX = (FileWatcherOSX *)callbackInfo;
    if (fwOSX->mDie)
    {
      pthread_mutex_lock(&fwOSX->mCallbackSync);

      // kill the run loop!
      FSEventStreamStop(fwOSX->mStream);
      FSEventStreamInvalidate(fwOSX->mStream);
      FSEventStreamRelease(fwOSX->mStream);

      CFRunLoopStop(fwOSX->mRunLoop);

      pthread_mutex_unlock(&fwOSX->mCallbackSync);
    }
  }

}
