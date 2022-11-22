#include "../../includes/win32/Watcher.h"

#include <sstream>

static
void stripNTPrefix(std::wstring &path) {
  if (path.rfind(L"\\\\?\\UNC\\", 0) != std::wstring::npos) {
    path.replace(0, 7, L"\\");
  } else if (path.rfind(L"\\\\?\\", 0) != std::wstring::npos) {
    path.erase(0, 4);
  }
}

static
std::wstring getWStringFileName(LPWSTR cFileName, DWORD length) {
  LPWSTR nullTerminatedFileName = new WCHAR[length + 1]();
  memcpy(nullTerminatedFileName, cFileName, length);
  std::wstring fileName = nullTerminatedFileName;
  delete[] nullTerminatedFileName;
  return fileName;
}

std::string Watcher::getUTF8Directory(std::wstring path) {
  std::wstring::size_type found = path.rfind('\\');
  std::wstringstream utf16DirectoryStream;

  utf16DirectoryStream << mPath;

  if (found != std::wstring::npos) {
      utf16DirectoryStream
        << "\\"
        << path.substr(0, found);
  }

  std::wstring uft16DirectoryString = utf16DirectoryStream.str();
  if (!mPathWasNtPrefixed) {
    // If we were the ones that prefixed the path, we should strip it
    // before returning it to the user
    stripNTPrefix(uft16DirectoryString);
  }

  int utf8length = WideCharToMultiByte(
      CP_UTF8,
      0,
      uft16DirectoryString.data(),
      -1,
      0,
      0,
      NULL,
      NULL
  );
  char *utf8CString = new char[utf8length];
  int failureToResolveToUTF8 = WideCharToMultiByte(
      CP_UTF8,
      0,
      uft16DirectoryString.data(),
      -1,
      utf8CString,
      utf8length,
      NULL,
      NULL
  );

  std::string utf8Directory = utf8CString;
  delete[] utf8CString;

  return utf8Directory;
}

static
std::string getUTF8FileName(std::wstring path) {
  std::wstring::size_type found = path.rfind('\\');
  if (found != std::wstring::npos) {
      path = path.substr(found + 1);
  }

  int utf8length = WideCharToMultiByte(
      CP_UTF8,
      0,
      path.data(),
      -1,
      0,
      0,
      NULL,
      NULL
      );

  // TODO: failure cases for widechar conversion
  char *utf8CString = new char[utf8length];
  int failureToResolveToUTF8 = WideCharToMultiByte(
      CP_UTF8,
      0,
      path.data(),
      -1,
      utf8CString,
      utf8length,
      NULL,
      NULL
      );

  std::string utf8Directory = utf8CString;
  delete[] utf8CString;

  return utf8Directory;
}

Watcher::Watcher(std::shared_ptr<EventQueue> queue, const std::wstring &path, bool pathWasNtPrefixed,
    const std::vector<std::wstring> &excludedPaths)
  : mRunning(false),
  mQueue(queue),
  mPath(path),
  mPathWasNtPrefixed(pathWasNtPrefixed),
  mExcludedPaths(excludedPaths)
{
  mDirectoryHandle = openDirectory(mPath);
  if (mDirectoryHandle == INVALID_HANDLE_VALUE) {
    setError("Failed to open directory");
  } else {
    ZeroMemory(&mOverlapped, sizeof(OVERLAPPED));
    mOverlapped.hEvent = this;
    resizeBuffers(1024 * 1024);
    mWatchedPath = getWatchedPathFromHandle();
    start();
  }
}

Watcher::~Watcher() {
  stop();
  std::lock_guard<std::mutex> lock(mHandleMutex);
  if (mDirectoryHandle != INVALID_HANDLE_VALUE) {
    CancelIo(mDirectoryHandle);
    CloseHandle(mDirectoryHandle);
  }
}

void Watcher::resizeBuffers(std::size_t size) {
  mReadBuffer.resize(size);
  mWriteBuffer.resize(size);
}

void Watcher::run() {
  while(mRunning) {
    SleepEx(INFINITE, true);
  }
}

bool Watcher::pollDirectoryChanges() {
  DWORD bytes = 0;

  if (!isRunning()) {
    return false;
  }

  std::lock_guard<std::mutex> lock(mHandleMutex);
  if (!ReadDirectoryChangesW(
    mDirectoryHandle,
    mWriteBuffer.data(),
    static_cast<DWORD>(mWriteBuffer.size()),
    TRUE,                           // recursive watching
    FILE_NOTIFY_CHANGE_FILE_NAME
    | FILE_NOTIFY_CHANGE_DIR_NAME
    | FILE_NOTIFY_CHANGE_LAST_WRITE,
    &bytes,                         // num bytes written
    &mOverlapped,
    [](DWORD errorCode, DWORD numBytes, LPOVERLAPPED overlapped) {
      auto watcher = reinterpret_cast<Watcher*>(overlapped->hEvent);
      watcher->eventCallback(errorCode);
    }))
  {
    setError("Service shutdown unexpectedly");
    return false;
  }

  return true;
}

HANDLE Watcher::openDirectory(const std::wstring &path) {
  return CreateFileW(
          path.data(),
          FILE_LIST_DIRECTORY,
          FILE_SHARE_READ
          | FILE_SHARE_WRITE
          | FILE_SHARE_DELETE,
          NULL,
          OPEN_EXISTING,
          FILE_FLAG_BACKUP_SEMANTICS
          | FILE_FLAG_OVERLAPPED,
          NULL
         );
}

void Watcher::reopenWathedFolder() {
  {
    std::lock_guard<std::mutex> lock(mHandleMutex);
    CancelIo(mDirectoryHandle);
    CloseHandle(mDirectoryHandle);
    mDirectoryHandle = openDirectory(mPath);
    if (mDirectoryHandle == INVALID_HANDLE_VALUE) {
      if(GetFileAttributesW(mWatchedPath.data()) == INVALID_FILE_ATTRIBUTES && GetLastError() == ERROR_PATH_NOT_FOUND) {
        setError("Service shutdown: root path changed (renamed or deleted)");
      } else {
        setError("Service shutdown unexpectedly");
      }
      return;
    }
  }
  pollDirectoryChanges();
}

void Watcher::eventCallback(DWORD errorCode) {
  if (errorCode != ERROR_SUCCESS) {
    if (errorCode == ERROR_NOTIFY_ENUM_DIR) {
      setError("Buffer filled up and service needs a restart");
    } else if (errorCode == ERROR_INVALID_PARAMETER) {
      // resize the buffers because we're over the network, 64kb is the max buffer size for networked transmission
      resizeBuffers(64 * 1024);

      if (!pollDirectoryChanges()) {
        setError("failed resizing buffers for network traffic");
      }
    } else if (errorCode == ERROR_ACCESS_DENIED) {
      // Access denied can happen when we try to delete the watched folder
      // In Windows Server 2019 and older the folder is not removed until folder handler is released
      // So the code will reopen the watched folder to release the handler and it will try to open it again
      reopenWathedFolder();
    } else {
      setError("Service shutdown unexpectedly");
    }
    return;
  }

  std::swap(mWriteBuffer, mReadBuffer);
  pollDirectoryChanges();
  handleEvents();
}

bool Watcher::isExcluded(const std::wstring &fileName) {
  std::wstring::size_type found = fileName.rfind('\\');

  std::wstring path;
  if (found != std::wstring::npos) {
    path = mPath + L"\\" + fileName.substr(0, found);
  } else {
    path = mPath + L"\\" + fileName;
  }
  for (const std::wstring &excludePath : mExcludedPaths) {
    if (path.size() >= excludePath.size() &&
      CompareStringEx(
      LOCALE_NAME_SYSTEM_DEFAULT,
      LINGUISTIC_IGNORECASE,
      path.data(),
      excludePath.size(),
      excludePath.data(),
      excludePath.size(),
      NULL,
      NULL,
      0) == CSTR_EQUAL) {
      return true;
    }
  }
  return false;
}

void Watcher::handleEvents() {
  BYTE *base = mReadBuffer.data();
  while (true) {
    PFILE_NOTIFY_INFORMATION info = (PFILE_NOTIFY_INFORMATION)base;
    std::wstring fileName = getWStringFileName(info->FileName, info->FileNameLength);

    if (!isExcluded(fileName)) {
      switch (info->Action) {
      case (FILE_ACTION_RENAMED_OLD_NAME):
        if (info->NextEntryOffset != 0) {
          base += info->NextEntryOffset;
          info = (PFILE_NOTIFY_INFORMATION)base;
          if (info->Action == FILE_ACTION_RENAMED_NEW_NAME) {
            std::wstring fileNameNew = getWStringFileName(info->FileName, info->FileNameLength);

            mQueue->enqueue(
              RENAMED,
              getUTF8Directory(fileName),
              getUTF8FileName(fileName),
              getUTF8Directory(fileName),
              getUTF8FileName(fileNameNew)
            );
          } else {
            mQueue->enqueue(DELETED, getUTF8Directory(fileName), getUTF8FileName(fileName));
          }
        } else {
          mQueue->enqueue(DELETED, getUTF8Directory(fileName), getUTF8FileName(fileName));
        }
        break;
      case FILE_ACTION_ADDED:
      case FILE_ACTION_RENAMED_NEW_NAME: // in the case we just receive a new name and no old name in the buffer
        mQueue->enqueue(CREATED, getUTF8Directory(fileName), getUTF8FileName(fileName));
        break;
      case FILE_ACTION_REMOVED:
        mQueue->enqueue(DELETED, getUTF8Directory(fileName), getUTF8FileName(fileName));
        break;
      case FILE_ACTION_MODIFIED:
      default:
        mQueue->enqueue(MODIFIED, getUTF8Directory(fileName), getUTF8FileName(fileName));
      };
    }

    if (info->NextEntryOffset == 0) {
      break;
    }
    base += info->NextEntryOffset;
  }
}

void Watcher::start() {
  mRunner = std::thread([this] {
    // mRunning is set to false in the d'tor
    mRunning = true;
    mIsRunningSemaphore.signal();
    run();
  });

  if (!mRunner.joinable()) {
    mRunning = false;
    return;
  }

  if (!mIsRunningSemaphore.waitFor(std::chrono::seconds(10))) {
    setError("Watcher is not started");
    return;
  }

  QueueUserAPC([](__in ULONG_PTR self) {
    auto watcher = reinterpret_cast<Watcher*>(self);
    watcher->pollDirectoryChanges();
    watcher->mHasStartedSemaphore.signal();
  }, mRunner.native_handle(), (ULONG_PTR)this);

  if (!mHasStartedSemaphore.waitFor(std::chrono::seconds(10))) {
    setError("Watcher is not started");
  }
}

void Watcher::stop() {
  if (isRunning()) {
    mRunning = false;
    // schedule a NOOP APC to force the running loop in `Watcher::run()` to wake
    // up, notice the changed `mRunning` and properly terminate the running loop
    QueueUserAPC([](__in ULONG_PTR) {}, mRunner.native_handle(), (ULONG_PTR)this);
    mRunner.join();
  }
}

void Watcher::setError(const std::string &error) {
  std::lock_guard<std::mutex> lock(mErrorMutex);
  mError = error;
}

std::string Watcher::getError() {
  if (!isRunning()) {
    return "Failed to start watcher";
  }

  if (mError.empty()) {
    checkWatchedPath();
  }

  std::lock_guard<std::mutex> lock(mErrorMutex);
  return mError;
}

std::wstring Watcher::getWatchedPathFromHandle() {
  std::lock_guard<std::mutex> lock(mHandleMutex);
  if (mDirectoryHandle == INVALID_HANDLE_VALUE) {
    return std::wstring();
  }
  DWORD pathLen = GetFinalPathNameByHandleW(mDirectoryHandle, NULL, 0, VOLUME_NAME_NT);
  if (pathLen == 0) {
    setError("Service shutdown: root path changed (renamed or deleted)");
    return std::wstring();
  }

  WCHAR* path = new WCHAR[pathLen];
  if (GetFinalPathNameByHandleW(mDirectoryHandle, path, pathLen, VOLUME_NAME_NT) != pathLen - 1) {
    setError("Service shutdown: root path changed (renamed or deleted)");
    delete[] path;
    return std::wstring();
  }

  std::wstring res(path);
  delete[] path;
  return res;
}

void Watcher::checkWatchedPath() {
  std::wstring path = getWatchedPathFromHandle();
  if (!path.empty() && path.compare(mWatchedPath) != 0) {
    setError("Service shutdown: root path changed (renamed or deleted)");
  }
}

void Watcher::updateExcludedPaths(const std::vector<std::wstring> &excludedPaths) {
  mExcludedPaths = excludedPaths;
}
