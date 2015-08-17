#include "../includes/FileWatcher32.h"

namespace NSFW {
  #pragma managed
  FSEventHandler::FSEventHandler(FileSystemWatcher ^parentFW, std::queue<Event> &eventsQueue, bool &watchFiles, bool &stopFlag)
    : mParentFW(parentFW), mEventsQueue(eventsQueue), mWatchFiles(watchFiles), mStopFlag(stopFlag) {}

  FSEventHandler::FSEventHandler(FileSystemWatcher ^parentFW, std::queue<Event> &eventsQueue, bool &watchFiles, bool &stopFlag, System::String ^fileName)
    : mParentFW(parentFW), mEventsQueue(eventsQueue), mFileName(fileName), mWatchFiles(watchFiles), mStopFlag(stopFlag) {}

  // Handles the generalized change event for changed/created/deleted and pushes event to queue
  void FSEventHandler::eventHandlerHelper(FileSystemEventArgs ^e, System::String ^action) {
    if (!mWatchFiles) {
      // Remove these handlers if the object is no longer listening (stop is called)
      return;
    }
    System::String ^eventFileName = Path::GetFileName(e->Name);
    if (!System::String::IsNullOrEmpty(mFileName) && eventFileName != mFileName) {
      return;
    }
    Event event;
    event.directory = (char*)(void*)Marshal::StringToHGlobalAnsi(Path::GetDirectoryName(e->FullPath));
    event.file = new std::string((char*)(void*)Marshal::StringToHGlobalAnsi(eventFileName));
    event.action = (char*)(void*)Marshal::StringToHGlobalAnsi(action);
    mEventsQueue.push(event);
  }
  FileSystemWatcher ^FSEventHandler::getParent() {
    return mParentFW;
  }

  bool &FSEventHandler::getStopFlag() {
    return mStopFlag;
  }

  bool &FSEventHandler::getWatchFiles() {
    return mWatchFiles;
  }

  void FSEventHandler::onChanged(Object ^source, FileSystemEventArgs ^e) {
    eventHandlerHelper(e, "CHANGED");
  }

  void FSEventHandler::onCreated(Object ^source, FileSystemEventArgs ^e) {
    eventHandlerHelper(e, "CREATED");
  }

  void FSEventHandler::onDeleted(Object ^source, FileSystemEventArgs ^e) {
    eventHandlerHelper(e, "DELETED");
  }

  // Specialized handler for renamed events, pushes to event queue
  void FSEventHandler::onRenamed(Object ^source, RenamedEventArgs ^e) {
    if (!mWatchFiles) {
      // Remove these handlers if the object is no longer listening (stop is called)
      return;
    }
    System::String ^eventFileName = Path::GetFileName(e->OldName);

    if (System::String::IsNullOrEmpty(mFileName)) {
      Event event;
      event.directory = (char*)(void*)Marshal::StringToHGlobalAnsi(Path::GetDirectoryName(e->FullPath));
      event.file = new std::string[2];
      event.file[0] = (char*)(void*)Marshal::StringToHGlobalAnsi(eventFileName);
      event.file[1] = (char*)(void*)Marshal::StringToHGlobalAnsi(Path::GetFileName(e->Name));
      event.action = "RENAMED";
      mEventsQueue.push(event);
      return;
    }

    if (mFileName == eventFileName) {
      Event event;
      event.directory = (char*)(void*)Marshal::StringToHGlobalAnsi(Path::GetDirectoryName(e->FullPath));
      event.file = new std::string((char*)(void*)Marshal::StringToHGlobalAnsi(eventFileName));
      event.action = "DELETED";
      mEventsQueue.push(event);
      return;
    }

    System::String ^newFileName = Path::GetFileName(e->Name);

    if (mFileName == newFileName) {
      Event event;
      event.directory = (char*)(void*)Marshal::StringToHGlobalAnsi(Path::GetDirectoryName(e->FullPath));
      event.file = new std::string((char*)(void*)Marshal::StringToHGlobalAnsi(newFileName));
      event.action = "CREATED";
      mEventsQueue.push(event);
      return;
    }
  }

  void FSEventHandler::rememberHandlers(
    FileSystemEventHandler ^changed,
    FileSystemEventHandler ^created,
    FileSystemEventHandler ^deleted,
    RenamedEventHandler ^renamed
  ) {
    mChanged = changed;
    mCreated = created;
    mDeleted = deleted;
    mRenamed = renamed;
  }

  void FSEventHandler::removeHandlers() {
    mParentFW->Changed -= mChanged;
    mParentFW->Created -= mCreated;
    mParentFW->Deleted -= mDeleted;
    mParentFW->Renamed -= mRenamed;
  }

  static void fileWatcherControl(Object ^data) {
    FSEventHandler ^handler = (FSEventHandler^)data;
    bool &watchFiles = handler->getWatchFiles();
    bool &stopFlag = handler->getStopFlag();
    FileSystemWatcher ^fsWatcher = handler->getParent();
    while(watchFiles) {
      Thread::Sleep(50);
    }
    fsWatcher->EnableRaisingEvents = false;
    handler->removeHandlers();
    delete fsWatcher;
    stopFlag = true;
  }

  // Creates the filewatcher and initializes the handlers.
  bool createFileWatcher(std::string path, std::queue<Event> &eventsQueue, bool &watchFiles, bool &stopFlag) {
    FileSystemWatcher ^fsWatcher;
    FSEventHandler ^handler;
    FileSystemEventHandler ^changed, ^created, ^deleted;
    RenamedEventHandler ^renamed;

    System::String ^gcPath = gcnew System::String(path.c_str());

    if (System::IO::Directory::Exists(gcPath)) {
      fsWatcher = gcnew FileSystemWatcher();
      fsWatcher->Path = gcPath;
      fsWatcher->IncludeSubdirectories = true;

      fsWatcher->NotifyFilter = static_cast<NotifyFilters>(
        NotifyFilters::FileName |
        NotifyFilters::DirectoryName |
        NotifyFilters::Attributes |
        NotifyFilters::LastWrite |
        NotifyFilters::Security |
        NotifyFilters::Size
      );

      handler = gcnew FSEventHandler(fsWatcher, eventsQueue, watchFiles, stopFlag);

    } else if (System::IO::File::Exists(gcPath)) {
      System::String ^gcFileName = Path::GetFileName(gcPath);
      gcPath = Path::GetDirectoryName(gcPath);

      fsWatcher = gcnew FileSystemWatcher();
      fsWatcher->Path = gcPath;
      fsWatcher->IncludeSubdirectories = false;

      fsWatcher->NotifyFilter = static_cast<NotifyFilters>(
        NotifyFilters::FileName |
        NotifyFilters::Attributes |
        NotifyFilters::LastWrite |
        NotifyFilters::Security |
        NotifyFilters::Size
      );

      handler = gcnew FSEventHandler(fsWatcher, eventsQueue, watchFiles, stopFlag, gcFileName);

    } else {
      return false;
    }

    changed = gcnew FileSystemEventHandler(handler, &FSEventHandler::onChanged);
    created = gcnew FileSystemEventHandler(handler, &FSEventHandler::onCreated);
    deleted = gcnew FileSystemEventHandler(handler, &FSEventHandler::onDeleted);
    renamed = gcnew RenamedEventHandler(handler, &FSEventHandler::onRenamed);

    // pass the handler delegates to the handler so that it can destroy them at a later time
    handler->rememberHandlers(changed, created, deleted, renamed);

    fsWatcher->Changed += changed;
    fsWatcher->Created += created;
    fsWatcher->Deleted += deleted;
    fsWatcher->Renamed += renamed;

    Thread ^oThread = gcnew Thread(gcnew ParameterizedThreadStart(&fileWatcherControl));
    oThread->Start(handler);

    fsWatcher->EnableRaisingEvents = true;

    return true;
  }

}
