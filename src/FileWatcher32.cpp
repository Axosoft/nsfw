#include "../includes/FileWatcher32.h"

namespace NSFW {
  #pragma managed
  FSEventHandler::FSEventHandler(FileSystemWatcher ^parentFW, std::queue<Event> &eventsQueue, bool &watchFiles, bool &stopFlag, Error &error)
    : mParentFW(parentFW), mEventsQueue(eventsQueue), mWatchFiles(watchFiles), mStopFlag(stopFlag), mError(error) {}

  FSEventHandler::FSEventHandler(FileSystemWatcher ^parentFW, std::queue<Event> &eventsQueue, bool &watchFiles, bool &stopFlag, Error &error, System::String ^fileName)
    : mParentFW(parentFW), mEventsQueue(eventsQueue), mFileName(fileName), mWatchFiles(watchFiles), mStopFlag(stopFlag), mError(error) {}

  // Handles the generalized change event for changed/created/deleted and pushes event to queue
  void FSEventHandler::eventHandlerHelper(FileSystemEventArgs ^e, System::String ^action) {
    try {
      if (!mWatchFiles || mError.status) {
        // Remove these handlers if the object is no longer listening (stop is called)
        return;
      }

      System::String ^eventFileName = getFileName(e->Name);
      if (!System::String::IsNullOrEmpty(mFileName) && eventFileName != mFileName) {
        return;
      }
      Event event;

      char *str = (char*)Marshal::StringToHGlobalAnsi(getDirectoryName(e->FullPath)).ToPointer();
      event.directory = str;
      Marshal::FreeHGlobal(IntPtr(str));

      str = (char*)Marshal::StringToHGlobalAnsi(eventFileName).ToPointer();
      event.file = new std::string(str);
      Marshal::FreeHGlobal(IntPtr(str));

      str = (char*)Marshal::StringToHGlobalAnsi(action).ToPointer();
      event.action = str;
      Marshal::FreeHGlobal(IntPtr(str));

      mEventsQueue.push(event);
    } catch (Exception ^e) {
      mError.status = true;
      mError.message = "An exception occurred in eventHandlerHelper";
    }
  }

  FileSystemWatcher ^FSEventHandler::getParent() {
    return mParentFW;
  }

  Error &FSEventHandler::getErrorStruct() {
    return mError;
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

  void FSEventHandler::onError(Object ^source, ErrorEventArgs ^e) {
    try {
      Exception ^exception = e->GetException();
      mError.status = true;
      char *str = (char*)Marshal::StringToHGlobalAnsi(exception->Message).ToPointer();
      mError.message = str;
      Marshal::FreeHGlobal(IntPtr(str));
    } catch (Exception ^e) {
      mError.status = true;
      mError.message = "An exception occurred in onError";
    }
  }

  // Specialized handler for renamed events, pushes to event queue
  void FSEventHandler::onRenamed(Object ^source, RenamedEventArgs ^e) {
    try {
      if (!mWatchFiles || mError.status) {
        // Remove these handlers if the object is no longer listening (stop is called)
        return;
      }
      System::String ^eventFileName = getFileName(e->OldName);
      if (System::String::IsNullOrEmpty(mFileName)) {
        Event event;
        char *str = (char*)Marshal::StringToHGlobalAnsi(getDirectoryName(e->FullPath)).ToPointer();
        event.directory = str;
        Marshal::FreeHGlobal(IntPtr(str));

        event.file = new std::string[2];

        str = (char*)Marshal::StringToHGlobalAnsi(eventFileName).ToPointer();
        event.file[0] = str;
        Marshal::FreeHGlobal(IntPtr(str));

        str = (char*)Marshal::StringToHGlobalAnsi(getFileName(e->Name)).ToPointer();
        event.file[1] = str;
        Marshal::FreeHGlobal(IntPtr(str));

        event.action = "RENAMED";
        mEventsQueue.push(event);
        return;
      }

      if (mFileName == eventFileName) {
        Event event;
        char *str = (char*)Marshal::StringToHGlobalAnsi(getDirectoryName(e->FullPath)).ToPointer();
        event.directory = str;
        Marshal::FreeHGlobal(IntPtr(str));

        str = (char*)Marshal::StringToHGlobalAnsi(eventFileName).ToPointer();
        event.file = new std::string(str);
        Marshal::FreeHGlobal(IntPtr(str));

        event.action = "DELETED";
        mEventsQueue.push(event);
        return;
      }

      System::String ^newFileName = getFileName(e->Name);

      if (mFileName == newFileName) {
        Event event;

        char *str = (char*)Marshal::StringToHGlobalAnsi(getDirectoryName(e->FullPath)).ToPointer();
        event.directory = str;
        Marshal::FreeHGlobal(IntPtr(str));

        str = (char*)Marshal::StringToHGlobalAnsi(newFileName).ToPointer();
        event.file = new std::string(str);
        Marshal::FreeHGlobal(IntPtr(str));

        event.action = "CREATED";
        mEventsQueue.push(event);
        return;
      }
    } catch (Exception ^e) {
      mError.status = true;
      mError.message = "An exception occurred in onRenamed";
    }
  }

  void FSEventHandler::rememberHandlers(
    FileSystemEventHandler ^changedHandler,
    FileSystemEventHandler ^createdHandler,
    FileSystemEventHandler ^deletedHandler,
    ErrorEventHandler ^errorHandler,
    RenamedEventHandler ^renamedHandler
  ) {
    mChangedHandler = changedHandler;
    mCreatedHandler = createdHandler;
    mDeletedHandler = deletedHandler;
    mErrorHandler = errorHandler;
    mRenamedHandler = renamedHandler;
  }

  void FSEventHandler::removeHandlers() {
    try {
      mParentFW->Changed -= mChangedHandler;
      mParentFW->Created -= mCreatedHandler;
      mParentFW->Deleted -= mDeletedHandler;
      mParentFW->Error -= mErrorHandler;
      mParentFW->Renamed -= mRenamedHandler;
    } catch (Exception ^e) {
      mError.status = true;
      mError.message = "An exception occurred in removeHandlers";
    }
  }

  static void fileWatcherControl(Object ^data) {
    FSEventHandler ^handler = (FSEventHandler^)data;
    Error &error = handler->getErrorStruct();
    try {
      bool &stopFlag = handler->getStopFlag();
      bool &watchFiles = handler->getWatchFiles();
      FileSystemWatcher ^fsWatcher = handler->getParent();
      while(watchFiles && !error.status) {
        Thread::Sleep(50);
      }
      fsWatcher->EnableRaisingEvents = false;
      handler->removeHandlers();
      delete fsWatcher;
      stopFlag = true;
    } catch (Exception ^e) {
      error.status = true;
      error.message = "An exception occurred in fileWatcherControl";
    }
  }

  // Creates the filewatcher and initializes the handlers.
  bool createFileWatcher(std::string path, std::queue<Event> &eventsQueue, bool &watchFiles, bool &stopFlag, Error &error) {
    FileSystemWatcher ^fsWatcher;
    FSEventHandler ^handler;
    FileSystemEventHandler ^changedHandler, ^createdHandler, ^deletedHandler;
    ErrorEventHandler ^errorHandler;
    RenamedEventHandler ^renamedHandler;

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

      handler = gcnew FSEventHandler(fsWatcher, eventsQueue, watchFiles, stopFlag, error);

    } else if (System::IO::File::Exists(gcPath)) {
      System::String ^gcFileName = getFileName(gcPath);
      gcPath = getDirectoryName(gcPath);

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

      handler = gcnew FSEventHandler(fsWatcher, eventsQueue, watchFiles, stopFlag, error, gcFileName);

    } else {
      return false;
    }

    changedHandler = gcnew FileSystemEventHandler(handler, &FSEventHandler::onChanged);
    createdHandler = gcnew FileSystemEventHandler(handler, &FSEventHandler::onCreated);
    deletedHandler = gcnew FileSystemEventHandler(handler, &FSEventHandler::onDeleted);
    errorHandler = gcnew ErrorEventHandler(handler, &FSEventHandler::onError);
    renamedHandler = gcnew RenamedEventHandler(handler, &FSEventHandler::onRenamed);

    // pass the handler delegates to the handler so that it can destroy them at a later time
    handler->rememberHandlers(changedHandler, createdHandler, deletedHandler, errorHandler, renamedHandler);

    fsWatcher->Changed += changedHandler;
    fsWatcher->Created += createdHandler;
    fsWatcher->Deleted += deletedHandler;
    fsWatcher->Error += errorHandler;
    fsWatcher->Renamed += renamedHandler;

    Thread ^oThread = gcnew Thread(gcnew ParameterizedThreadStart(&fileWatcherControl));
    oThread->Start(handler);

    fsWatcher->EnableRaisingEvents = true;

    return true;
  }

  System::String^ getDirectoryName(System::String^ path) {
    wchar_t delim = '\\';
    array<System::String^>^ tokens = path->Split(delim);

    if (path[path->Length - 1] == delim) {
      if (tokens->Length == 2 && System::String::IsNullOrEmpty(tokens[1])
       || tokens->Length < 2) {
        return gcnew System::String("");
      } else {
        return path->Substring(0, path->Length - 1);
      }
    } else {
      return path->Substring(0, path->LastIndexOf(delim));
    }
  }

  System::String^ getFileName(System::String^ path) {
    wchar_t delim = '\\';
    if (path->LastIndexOf(delim) == path->Length - 1) {
      return gcnew System::String("");
    } else {
      return path->Substring(path->LastIndexOf(delim) + 1);
    }
  }
}
