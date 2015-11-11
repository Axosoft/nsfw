#include "../includes/FileWatcherLinux.h"
#include <iostream>

namespace NSFW {
  FileWatcherLinux::FileWatcherLinux(std::string path, EventQueue &eventQueue, bool &watchFiles, Error &error)
    : mError(error), mEventQueue(eventQueue), mInotify(0), mPath(path), mWatchFiles(watchFiles)
  {
    // strip trailing slash
    if (mPath[mPath.length() - 1] == '/')
    {
      mPath = mPath.substr(0, mPath.length() - 1);
    }

    pthread_mutexattr_t attr;
    if (pthread_mutexattr_init(&attr) == 0)
    {
      pthread_mutex_init(&mMainLoopSync, &attr);
    }
  }

  FileWatcherLinux::~FileWatcherLinux()
  {
    pthread_mutex_destroy(&mMainLoopSync);
  }

  bool FileWatcherLinux::addDirectoryEvent(inotify_event *inEvent)
  {
    std::queue<std::string> dirQueue;

    Directory *dir = new Directory,
              *root = mWDtoDirNode[inEvent->wd];
    dir->path = root->path + "/" + root->name;
    dir->name = inEvent->name;
    root->childDirectories[dir->name] = dir;
    dir->parent = root;

    std::string fullPath = dir->path + "/" + dir->name;

    int attributes =  IN_ATTRIB |
                      IN_CREATE |
                      IN_DELETE |
                      IN_MODIFY |
                      IN_MOVED_FROM |
                      IN_MOVED_TO;
    dir->watchDescriptor = inotify_add_watch(
      mInotify,
      fullPath.c_str(),
      attributes
    );

    if (dir->watchDescriptor < 0)
    {
      if (errno == ENOSPC)
      {
        setErrorMessage("Increase capacity for inotify watchers");
      }
      else if (errno == ENOMEM)
      {
        setErrorMessage("Kernel ran out of memory for inotify watchers");
      }
      else if (errno == EBADF)
      {
        setErrorMessage("Invalid file descriptor");
      }

      destroyWatchTree(mDirTree);
      return false;
    }

    mWDtoDirNode[dir->watchDescriptor] = dir;

    dirQueue.push(fullPath);

    while(!dirQueue.empty())
    {
      std::string dirPath = dirQueue.front();
      dirent ** directoryContents = NULL;

      int n = scandir(
        dirPath.c_str(),
        &directoryContents,
        NULL,
        alphasort
      );

      if (n < 0)
      {
        dirQueue.pop();
        continue;
      }

      for (int i = 0; i < n; ++i)
      {
        if (
          !strcmp(directoryContents[i]->d_name, ".") ||
          !strcmp(directoryContents[i]->d_name, "..")
        ) {
          continue; // skip navigation folder
        }

        // Certain *nix do not support dirent->d_type and may return DT_UNKOWN for every file returned in scandir
        // in order to make this work on all *nix, we need to stat the file to determine if it is a directory
        std::string filePath = dirPath + "/" + directoryContents[i]->d_name;

        struct stat file;

        if (stat(filePath.c_str(), &file) < 0)
        {
          continue;
        }

        if (S_ISDIR(file.st_mode))
        {
          dirQueue.push(filePath);
        }

        addEvent(
          CREATED,
          dirPath,
          directoryContents[i]->d_name
        );
      }

      for (int i = 0; i < n; ++i)
      {
        delete directoryContents[i];
      }

      delete[] directoryContents;

      dirQueue.pop();
    }

    return true;
  }

  void FileWatcherLinux::addEvent(Action action, inotify_event *inEvent)
  {
    Directory *parent = mWDtoDirNode[inEvent->wd];
    mEventQueue.enqueue(
      action,
      parent->path + "/" + parent->name,
      inEvent->name
    );
  }

  void FileWatcherLinux::addEvent(
    Action action,
    std::string directory,
    std::string fileA,
    std::string fileB
  ) {
    mEventQueue.enqueue(
      action,
      directory,
      fileA,
      fileB
    );
  }

  bool FileWatcherLinux::buildWatchDirectory()
  {
    Directory *watchDir = new Directory;
    // strip name from path
    size_t lastSlash = mPath.find_last_of("/");
    if (lastSlash != std::string::npos)
    {
      watchDir->name = mPath.substr(lastSlash + 1);
      watchDir->path = mPath.substr(0, lastSlash);
    }
    else
    {
      delete watchDir;
      return false;
    }

    // set watch descriptor for the parent directory of the file
    watchDir->watchDescriptor = inotify_add_watch(
      mInotify,
      watchDir->path.c_str(),
      IN_ATTRIB |
      IN_CREATE |
      IN_DELETE |
      IN_MODIFY |
      IN_MOVED_FROM |
      IN_MOVED_TO
    );

    // if it fails, we'll return an error
    if (watchDir->watchDescriptor < 0)
    {
      delete watchDir;
      return false;
    }

    mDirTree = watchDir;
    return true;
  }

  // creates a tree of watch descriptors around mPath
  bool FileWatcherLinux::buildWatchTree()
  {
    std::queue<Directory *> dirQueue;
    Directory *topRoot = new Directory;
    topRoot->parent = NULL;

    size_t lastSlash = mPath.find_last_of("/");
    if (lastSlash != std::string::npos)
    {
      topRoot->name = mPath.substr(lastSlash + 1);
      topRoot->path = mPath.substr(0, lastSlash);
    }
    else
    {
      topRoot->name = "";
      topRoot->path = "/";
    }

    topRoot->watchDescriptor = NO_WATCH;

    dirQueue.push(topRoot);

    while (!dirQueue.empty())
    {
      Directory *root = dirQueue.front();
      dirent ** directoryContents = NULL;
      std::string fullPath = root->path + "/" + root->name;

      // set up the descriptor for this diretctory
      int attributes;
      if (root == topRoot)
      {
        attributes =  IN_ATTRIB |
                      IN_CREATE |
                      IN_DELETE |
                      IN_MODIFY |
                      IN_MOVED_FROM |
                      IN_MOVED_TO |
                      IN_DELETE_SELF;
      }
      else
      {
        attributes =  IN_ATTRIB |
                      IN_CREATE |
                      IN_DELETE |
                      IN_MODIFY |
                      IN_MOVED_FROM |
                      IN_MOVED_TO;
      }

      root->watchDescriptor = inotify_add_watch(
        mInotify,
        fullPath.c_str(),
        attributes
      );

      if (root->watchDescriptor < 0)
      {
        if (errno == ENOSPC)
        {
          setErrorMessage("Increase capacity for inotify watchers");
        }
        else if (errno == ENOMEM)
        {
          setErrorMessage("Kernel ran out of memory for inotify watchers");
        }
        else if (errno == EBADF)
        {
          setErrorMessage("Invalid file descriptor");
        }

        destroyWatchTree(topRoot);
        return false;
      }

      int n = scandir(
        fullPath.c_str(),
        &directoryContents,
        NULL,
        alphasort
      );

      if (n < 0) {
        if (fcntl(mInotify, F_GETFD >= 0))
        {
          inotify_rm_watch(mInotify, root->watchDescriptor);
        }

        if (root->parent)
        {
          root->parent->childDirectories.erase(root->name);
          dirQueue.pop();
          delete root;
          continue;
        }
        else
        {
          // top level failure
          delete topRoot;
          return false;
        }
      }

      mWDtoDirNode[root->watchDescriptor] = root;

      // find all the directories within this directory
      // this breaks the alphabetical sorting of directories
      for (int i = 0; i < n; ++i)
      {
        if (
          !strcmp(directoryContents[i]->d_name, ".") ||
          !strcmp(directoryContents[i]->d_name, "..")
        ) {
          continue; // skip navigation folder
        }

        // Certain *nix do not support dirent->d_type and may return DT_UNKOWN for every file returned in scandir
        // in order to make this work on all *nix, we need to stat the file to determine if it is a directory
        std::string filePath = root->path + "/" + root->name + "/" + directoryContents[i]->d_name;

        struct stat file;

        if (stat(filePath.c_str(), &file) < 0)
        {
          continue;
        }

        if (S_ISDIR(file.st_mode))
        {
          // create the directory struct for this directory and add a reference of this directory to its root
          Directory *dir = new Directory;
          dir->path = root->path + "/" + root->name;
          dir->name = directoryContents[i]->d_name;
          dir->watchDescriptor = NO_WATCH;
          root->childDirectories[dir->name] = dir;
          dir->parent = root;
          dirQueue.push(dir);
        }
        else
        {
          root->files.insert(directoryContents[i]->d_name);
        }
      }

      for (int i = 0; i < n; ++i)
      {
        delete directoryContents[i];
      }

      delete[] directoryContents;

      dirQueue.pop();
    }

    mDirTree = topRoot;
    return true;
  }

  void FileWatcherLinux::destroyWatchTree(Directory *tree, std::set<Directory *> *cleanUp)
  {
    std::queue<Directory *> dirQueue;

    // unlink this node from parent
    if (tree->parent)
    {
      tree->parent->childDirectories.erase(tree->name);
      tree->parent = NULL;
    }

    dirQueue.push(tree);

    while (!dirQueue.empty())
    {
      Directory *root = dirQueue.front();

      // remove watch if possible
      if (fcntl(mInotify, F_GETFD >= 0))
      {
        inotify_rm_watch(mInotify, root->watchDescriptor);
      }

      // Add directories to the queue to continue listing events
      for (
        std::map<std::string, Directory *>::iterator dirIter = root->childDirectories.begin();
        dirIter != root->childDirectories.end();
        ++dirIter
      ) {
        dirQueue.push(dirIter->second);
        dirIter->second->parent = NULL;
      }

      root->childDirectories.clear();
      dirQueue.pop();

      if (cleanUp)
      {
        root->watchDescriptor = DEAD_NODE;
        cleanUp->insert(root);
      }
      else
      {
        delete root;
      }
    }
  }

  // This method expects a dead child
  Directory *FileWatcherLinux::findFirstDeadAncestor(Directory *child)
  {
    Directory *current = child,
              *ancestor = child->parent;
    struct stat file;

    while (ancestor)
    {
      std::string fullPath = ancestor->path + "/" + ancestor->name;

      if (stat(fullPath.c_str(), &file) == 0)
      {
        return current;
      }
    }

    return mDirTree;
  }

  std::string FileWatcherLinux::getPath()
  {
    return mPath;
  }

  void *FileWatcherLinux::mainLoop(void *params)
  {
    FileWatcherLinux *fwLinux = (FileWatcherLinux *)params;
    struct stat file;

    if (stat(fwLinux->getPath().c_str(), &file) < 0)
    {
      fwLinux->setErrorMessage("Access is denied");
      return NULL;
    }

    if (S_ISDIR(file.st_mode))
    {
      // build the directory tree before listening for events
      if (!fwLinux->buildWatchTree())
      {
        fwLinux->setErrorMessage("Access is denied");
        return NULL;
      }

      fwLinux->processDirectoryEvents();
    }
    else if (S_ISREG(file.st_mode))
    {
      if (!fwLinux->buildWatchDirectory())
      {
        fwLinux->setErrorMessage("Access is denied");
        return NULL;
      }

      fwLinux->processFileEvents();
    }
    else
    {
      fwLinux->setErrorMessage("Access is denied");
    }

    return NULL;
  }

  void FileWatcherLinux::processDirectoryEvents()
  {
    char buffer[8192];
    int watchDescriptor = NO_WATCH;
    unsigned int bytesRead, position = 0, cookie = 0;
    Event lastMovedFromEvent;

    while(
      mWatchFiles &&
      (bytesRead = read(mInotify, &buffer, 8192)) > 0
    ) {
      pthread_mutex_lock(&mMainLoopSync);

      if (!mWatchFiles || mError.status)
      { // we've been stopped
        pthread_mutex_lock(&mMainLoopSync);
        return;
      }

      inotify_event *inEvent;
      do
      {
        if (mError.status)
        {
          return;
        }

        inEvent = (inotify_event *)(buffer + position);
        Event event;

        // if the event is not a moved to event and the cookie exists
        // we should reset the cookie and push the last moved from event
        if (
          cookie != 0 &&
          inEvent->mask != IN_MOVED_TO
        ) {
          addEvent(
            lastMovedFromEvent.action,
            lastMovedFromEvent.directory,
            lastMovedFromEvent.fileA
          );
          cookie = 0;
          watchDescriptor = NO_WATCH;
        }
        bool isDir = inEvent->mask & IN_ISDIR;
        inEvent->mask = isDir ? inEvent->mask ^ IN_ISDIR : inEvent->mask;

        switch(inEvent->mask)
        {
          case IN_ATTRIB:
          case IN_MODIFY:
            if (*inEvent->name > 31)
            { // ignore control characters
              addEvent(MODIFIED, inEvent);
            }
            break;
          case IN_CREATE:
          {
            Directory *parent = mWDtoDirNode[inEvent->wd];
            // check stats on the item CREATED
            // if it is a dir, create a watch for all of its directories
            if (
              isDir &&
              parent->childDirectories.find(inEvent->name) == parent->childDirectories.end() &&
              addDirectoryEvent(inEvent)
            ) {
              addEvent(CREATED, inEvent);
              break;
            }

            if (
              !isDir &&
              parent->files.find(inEvent->name) == parent->files.end()
            ) {
              parent->files.insert(inEvent->name);
              addEvent(CREATED, inEvent);
            }
            break;
          }
          case IN_DELETE:
          {
            if (!isDir)
            {
              mWDtoDirNode[inEvent->wd]->files.erase(inEvent->name);
            }

            addEvent(DELETED, inEvent);
            break;
          }
          case IN_MOVED_FROM:
            fd_set checkWD;
            FD_ZERO(&checkWD);
            FD_SET(mInotify, &checkWD);
            timeval timeout;
            timeout.tv_sec = 0;
            timeout.tv_usec = 250000;

            if (!isDir)
            {
              mWDtoDirNode[inEvent->wd]->files.erase(inEvent->name);
            }

            if (
              position + sizeof(struct inotify_event) + inEvent->len < bytesRead ||
              select(mInotify+1, &checkWD, 0, 0, &timeout) > 0
            ) {
              lastMovedFromEvent.action = DELETED;
              lastMovedFromEvent.directory = mWDtoDirNode[inEvent->wd]->path;
              lastMovedFromEvent.fileA = inEvent->name;
              cookie = inEvent->cookie;
              watchDescriptor = inEvent->wd;
            }
            else
            {
              addEvent(DELETED, inEvent);
            }
            break;
          case IN_MOVED_TO:
            if (!isDir)
            {
              mWDtoDirNode[inEvent->wd]->files.insert(inEvent->name);
            }
            // check if this is a move event
            if (
              cookie != 0 &&
              inEvent->cookie == cookie && inEvent->wd == watchDescriptor
            ) {
              cookie = 0;
              watchDescriptor = -1;
              addEvent(
                RENAMED,
                mWDtoDirNode[inEvent->wd]->path,
                lastMovedFromEvent.fileA,
                inEvent->name
              );
            }
            else
            {
              addEvent(CREATED, inEvent);
            }
            break;
          case IN_DELETE_SELF:
            setErrorMessage("Access is denied");
            pthread_mutex_unlock(&mMainLoopSync);
            return;
        }
      } while ((position += sizeof(struct inotify_event) + inEvent->len) < bytesRead);

      if (!refreshWatchTree())
      {
        // TODO: ensure this is safe
        pthread_mutex_unlock(&mMainLoopSync);
        return;
      }

      pthread_mutex_unlock(&mMainLoopSync);
      position = 0;
    }
  }

  void FileWatcherLinux::processFileEvents()
  {
    char buffer[8192];
    unsigned int bytesRead, position = 0;

    while(
      mWatchFiles &&
      (bytesRead = read(mInotify, &buffer, 8192)) > 0
    ) {
      inotify_event *inEvent;
      do
      {
        inEvent = (inotify_event *)(buffer + position);
        Event event;
        if (strcmp(inEvent->name, mDirTree->name.c_str()))
        {
          continue;
        }
        switch(inEvent->mask)
        {
          case IN_ATTRIB:
          case IN_MODIFY:
            addEvent(
              MODIFIED,
              mDirTree->path,
              mDirTree->name
            );
            break;
          case IN_MOVED_TO:
          case IN_CREATE:
            addEvent(
              CREATED,
              mDirTree->path,
              mDirTree->name
            );
            break;
          case IN_MOVED_FROM:
          case IN_DELETE:
            addEvent(
              DELETED,
              mDirTree->path,
              mDirTree->name
            );
            break;
        }
      } while ((position += sizeof(struct inotify_event) + inEvent->len) < bytesRead);
      position = 0;
    }
  }

  // prunes the watch tree and insures that all watch descriptors are up to date
  bool FileWatcherLinux::refreshWatchTree()
  {
    std::set<Directory *> cleanUp;
    std::queue<Directory *> dirQueue;
    dirQueue.push(mDirTree);

    while (!dirQueue.empty())
    {
      Directory *root = dirQueue.front();

      if (root->watchDescriptor == DEAD_NODE)
      {
        dirQueue.pop();
        continue;
      }

      dirent ** directoryContents = NULL;
      std::string fullPath = root->path + "/" + root->name;

      // check to see if this directory still exists
      int n = scandir(
        fullPath.c_str(),
        &directoryContents,
        NULL,
        alphasort
      );

      // if it does not exist we should remove the watch associated with the directory
      // and we should remove the directory from the parent's list
      // for the case that the top of the tree has failed to scan correctly
      if (n < 0)
      {
        if (fcntl(mInotify, F_GETFD >= 0))
        {
          inotify_rm_watch(mInotify, root->watchDescriptor);
        }

        if (root->parent)
        {
          Directory *deadRoot = findFirstDeadAncestor(root);

          if (deadRoot == mDirTree)
          {
            // TODO: make into small function
            setErrorMessage("Access is denied");
            destroyWatchTree(root);
            mDirTree = NULL;
            return false;
          }

          destroyWatchTree(deadRoot, &cleanUp);
          continue;
        }
        else
        {
          setErrorMessage("Access is denied");
          destroyWatchTree(root);
          mDirTree = NULL;
          return false;
        }
      }

      for (int i = 0; i < n; ++i)
      {
        if (
          !strcmp(directoryContents[i]->d_name, ".") ||
          !strcmp(directoryContents[i]->d_name, "..")
        ) {
          continue; // skip navigation folder
        }

        // Certain *nix do not support dirent->d_type and may return DT_UNKOWN for every file returned in scandir
        // in order to make this work on all *nix, we need to stat the file to determine if it is a directory
        std::string filePath = root->path + "/" + root->name + "/" + directoryContents[i]->d_name;

        struct stat file;

        if (stat(filePath.c_str(), &file) < 0)
        {
          continue;
        }

        if (
          !S_ISDIR(file.st_mode) &&
          root->childDirectories.find(directoryContents[i]->d_name) == root->childDirectories.end()
        ) {
          int attributes =  IN_ATTRIB |
                            IN_CREATE |
                            IN_DELETE |
                            IN_MODIFY |
                            IN_MOVED_FROM |
                            IN_MOVED_TO;
          int watchDescriptor = inotify_add_watch(
            mInotify,
            filePath.c_str(),
            attributes
          );

          if (watchDescriptor < 0)
          {
            if (errno == ENOSPC)
            {
              setErrorMessage("Increase capacity for inotify watchers");
            }
            else if (errno == ENOMEM)
            {
              setErrorMessage("Kernel ran out of memory for inotify watchers");
            }
            else if (errno == EBADF)
            {
              setErrorMessage("Invalid file descriptor");
            }

            for (int i = 0; i < n; ++i)
            {
              delete directoryContents[i];
            }

            delete[] directoryContents;

            destroyWatchTree(mDirTree);
            mDirTree = NULL;
            return false;
          }

          // create the directory struct for this directory and add a reference of this directory to its root
          Directory *dir = new Directory;
          dir->path = root->path + "/" + root->name;
          dir->name = directoryContents[i]->d_name;
          dir->watchDescriptor = watchDescriptor;
          root->childDirectories[dir->name] = dir;
          dir->parent = root;

          // before we add this we should check to make sure it doesn't already exist.
          mWDtoDirNode[watchDescriptor] = dir;
          dirQueue.push(dir);
        }
      }

      for (int i = 0; i < n; ++i)
      {
        delete directoryContents[i];
      }

      delete[] directoryContents;

      dirQueue.pop();
    }

    for (
      std::set<Directory *>::iterator dirIter = cleanUp.begin();
      dirIter != cleanUp.end();
      ++dirIter
    ) {
      delete *dirIter;
    }

    return true;
  }

  void FileWatcherLinux::setErrorMessage(std::string message)
  {
    mError.status = true;
    mError.message = message;
  }

  bool FileWatcherLinux::start()
  {
    mInotify = inotify_init();
    if (mInotify < 0)
    {
      return false;
    }

    if (
      mWatchFiles &&
      pthread_create(
        &mThread,
        0,
        &FileWatcherLinux::mainLoop,
        (void *)this
      )
    ) {
      return true;
    }
    else
    {
      return false;
    }
  }

  void FileWatcherLinux::stop()
  {
    pthread_mutex_lock(&mMainLoopSync);

    if (mDirTree != NULL)
    {
      destroyWatchTree(mDirTree);
      mDirTree = NULL;
    }

    pthread_mutex_unlock(&mMainLoopSync);

    int t;
    pthread_setcancelstate(PTHREAD_CANCEL_ASYNCHRONOUS, &t);
    pthread_cancel(mThread);

    close(mInotify);
  }
}
