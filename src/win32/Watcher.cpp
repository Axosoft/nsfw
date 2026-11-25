#include "../../includes/win32/Watcher.h"

static
std::wstring getWStringFileName(LPWSTR cFileName, DWORD length) {
  LPWSTR nullTerminatedFileName = new WCHAR[length + 1]();
  memcpy(nullTerminatedFileName, cFileName, length);
  std::wstring fileName = nullTerminatedFileName;
  delete[] nullTerminatedFileName;
  return fileName;
}

static std::wstring joinPath(const std::wstring &directory, const std::wstring &relativePath) {
  const auto last = directory.find_last_not_of(L"\\/");
  return directory.substr(0, last == std::wstring::npos ? 0 : last + 1) + L"\\" + relativePath;
}

std::string Watcher::getUTF8Directory(std::wstring path) {
  std::wstring::size_type found = path.rfind('\\');
  std::wstring uft16DirectoryString = found == std::wstring::npos
    ? mPath
    : joinPath(mPath, path.substr(0, found));

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

static const unsigned MAX_ABORTED_READS = 3;

static std::string describeError(const std::string &message, DWORD errorCode) {
  LPWSTR text = NULL;
  const DWORD length = FormatMessageW(
    FORMAT_MESSAGE_ALLOCATE_BUFFER | FORMAT_MESSAGE_FROM_SYSTEM | FORMAT_MESSAGE_IGNORE_INSERTS,
    NULL,
    errorCode,
    0,
    reinterpret_cast<LPWSTR>(&text),
    0,
    NULL
  );
  std::wstring description = length == 0 ? std::wstring() : std::wstring(text, length);
  LocalFree(text);
  const auto last = description.find_last_not_of(L" .\r\n");
  description.erase(last == std::wstring::npos ? 0 : last + 1);

  std::string result = message + ": ";
  if (!description.empty()) {
    result += filterpaths::fromWide(description) + " ";
  }
  return result + "(Windows error " + std::to_string(errorCode) + ")";
}

Watcher::Watcher(std::shared_ptr<EventQueue> queue, const std::wstring &path,
    const std::vector<std::wstring> &excludedPaths)
  : mRunning(false),
  mQueue(queue),
  mPath(path),
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
    openParent();
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
  if (mParentHandle != INVALID_HANDLE_VALUE) {
    CancelIo(mParentHandle);
    CloseHandle(mParentHandle);
  }
}

void Watcher::resizeBuffers(std::size_t size) {
  mReadBuffer.resize(size);
  mWriteBuffer.resize(size);
}

void Watcher::run() {
  while(mRunning) {
    if (!mPollWatchedPath) {
      SleepEx(INFINITE, true);
    } else if (SleepEx(1000, true) == 0 && mRunning && !hasError()) {
      checkWatchedPath();
    }
  }
}

// The parent of the directory the handle leads to, not of the path as given: through a junction, a symlink or a
// subst drive, renaming the directory changes a name in that one.
void Watcher::openParent() {
  std::wstring path;
  {
    std::lock_guard<std::mutex> lock(mHandleMutex);
    DWORD length = GetFinalPathNameByHandleW(mDirectoryHandle, NULL, 0, VOLUME_NAME_DOS);
    if (length != 0) {
      path.resize(length);
      length = GetFinalPathNameByHandleW(mDirectoryHandle, &path[0], length, VOLUME_NAME_DOS);
      path.resize(length < path.size() ? length : 0);
    }
  }
  // \\?\C:\dir or \\?\UNC\server\share\dir. A drive or share root has no parent, and can't be renamed.
  const std::wstring unc = L"\\\\?\\UNC\\";
  const std::size_t rootSeparators = path.compare(0, unc.size(), unc) == 0 ? 5 : 3;
  const auto last = path.find_last_not_of(L"\\");
  const auto separator = last == std::wstring::npos ? last : path.find_last_of(L"\\", last);
  std::size_t separators = 0;
  for (std::size_t i = 0; separator != std::wstring::npos && i <= separator; ++i) {
    if (path[i] == L'\\') ++separators;
  }
  if (!path.empty() && separators <= rootSeparators) {
    return;
  }
  if (separator != std::wstring::npos) {
    mParentHandle = openDirectory(path.substr(0, separator + 1));
  }
  if (mParentHandle == INVALID_HANDLE_VALUE) {
    mPollWatchedPath = true;
    return;
  }
  ZeroMemory(&mParentOverlapped, sizeof(OVERLAPPED));
  mParentOverlapped.hEvent = this;
  mParentBuffer.resize(4096);
}

bool Watcher::hasError() {
  std::lock_guard<std::mutex> lock(mErrorMutex);
  return !mError.empty();
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
      watcher->eventCallback(errorCode, numBytes);
    }))
  {
    const DWORD error = GetLastError();
    setError(describeError("Service shutdown unexpectedly", error));
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
      const DWORD openError = GetLastError();
      if(GetFileAttributesW(mWatchedPath.data()) == INVALID_FILE_ATTRIBUTES && GetLastError() == ERROR_PATH_NOT_FOUND) {
        setError("Service shutdown: root path changed (renamed or deleted)");
      } else {
        setError(describeError("Service shutdown unexpectedly", openError));
      }
      return;
    }
  }
  pollDirectoryChanges();
}

// Only the directory names in the parent: what renames the watched directory, or moves it out. Each change has the
// path the watched directory's handle leads to compared again, instead of checking it on a timer.
void Watcher::watchParent() {
  std::lock_guard<std::mutex> lock(mHandleMutex);
  if (mParentHandle == INVALID_HANDLE_VALUE) {
    return;
  }
  DWORD bytes = 0;
  if (!ReadDirectoryChangesW(
    mParentHandle,
    mParentBuffer.data(),
    static_cast<DWORD>(mParentBuffer.size()),
    FALSE,
    FILE_NOTIFY_CHANGE_DIR_NAME,
    &bytes,
    &mParentOverlapped,
    [](DWORD errorCode, DWORD numBytes, LPOVERLAPPED overlapped) {
      auto watcher = reinterpret_cast<Watcher*>(overlapped->hEvent);
      watcher->parentChanged(errorCode);
    }))
  {
    CloseHandle(mParentHandle);
    mParentHandle = INVALID_HANDLE_VALUE;
    mPollWatchedPath = true;
  }
}

void Watcher::parentChanged(DWORD errorCode) {
  if (errorCode == ERROR_OPERATION_ABORTED || !isRunning()) {
    return;
  }
  checkWatchedPath();
  if (hasError()) {
    return;
  }
  // An overflow only lost names: the check above covers them. Any other failure, the parent being deleted say, ends
  // its watch, so the handle doesn't keep it from going, and the path is checked on a timer instead.
  if (errorCode != ERROR_SUCCESS && errorCode != ERROR_NOTIFY_ENUM_DIR) {
    std::lock_guard<std::mutex> lock(mHandleMutex);
    CloseHandle(mParentHandle);
    mParentHandle = INVALID_HANDLE_VALUE;
    mPollWatchedPath = true;
    return;
  }
  watchParent();
}

void Watcher::eventCallback(DWORD errorCode, DWORD numBytes) {
  if (errorCode == ERROR_SUCCESS && numBytes == 0) {
    // Nothing was written because more changed than the buffer holds: those changes are lost, as when the system says
    // so with ERROR_NOTIFY_ENUM_DIR.
    errorCode = ERROR_NOTIFY_ENUM_DIR;
  }
  if (errorCode != ERROR_SUCCESS) {
    if (errorCode == ERROR_OPERATION_ABORTED) {
      // CancelIo() or closing the handle aborted the read. Whoever did it while stopping wants no more reads, and no
      // error either; if the watcher still runs, the read is issued again. Reads that keep being aborted are reported,
      // instead of being issued again forever without a word.
      if (isRunning()) {
        if (++mAbortedReads < MAX_ABORTED_READS) {
          pollDirectoryChanges();
        } else {
          setError(describeError("Service shutdown unexpectedly", errorCode));
        }
      }
    } else if (errorCode == ERROR_NOTIFY_ENUM_DIR) {
      setError("Buffer filled up and service needs a restart");
    } else if (errorCode == ERROR_INVALID_PARAMETER) {
      // resize the buffers because we're over the network, 64kb is the max buffer size for networked transmission
      resizeBuffers(64 * 1024);

      pollDirectoryChanges();
    } else if (errorCode == ERROR_ACCESS_DENIED) {
      // Access denied can happen when we try to delete the watched folder
      // In Windows Server 2019 and older the folder is not removed until folder handler is released
      // So the code will reopen the watched folder to release the handler and it will try to open it again
      reopenWathedFolder();
    } else {
      setError(describeError("Service shutdown unexpectedly", errorCode));
    }
    return;
  }

  mAbortedReads = 0;
  std::swap(mWriteBuffer, mReadBuffer);
  pollDirectoryChanges();
  // Not under a path that leads elsewhere now: the run ends with the error, and the batch with it.
  checkWatchedPath();
  if (hasError()) {
    return;
  }
  handleEvents();
}

static bool isSeparator(wchar_t character) {
  return character == L'\\' || character == L'/';
}

bool Watcher::isExcluded(const std::wstring &fileName) {
  std::wstring::size_type found = fileName.rfind('\\');

  std::wstring path;
  if (found != std::wstring::npos) {
    path = joinPath(mPath, fileName.substr(0, found));
  } else {
    path = joinPath(mPath, fileName);
  }
  std::lock_guard<std::mutex> lock(mExcludedPathsMutex);
  for (const std::wstring &excludePath : mExcludedPaths) {
    if (
      excludePath.empty() ||
      path.size() < excludePath.size() ||
      CompareStringEx(
        LOCALE_NAME_SYSTEM_DEFAULT,
        LINGUISTIC_IGNORECASE,
        path.data(),
        static_cast<int>(excludePath.size()),
        excludePath.data(),
        static_cast<int>(excludePath.size()),
        NULL,
        NULL,
        0
      ) != CSTR_EQUAL
    ) {
      continue;
    }
    // Only whole path components match: excluding C:\w\foo must not exclude C:\w\foobar.
    if (
      path.size() == excludePath.size() ||
      isSeparator(excludePath.back()) ||
      isSeparator(path[excludePath.size()])
    ) {
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
              getUTF8Directory(fileNameNew),
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
    // From this thread: the completion routines run on the one that issued the read.
    watcher->watchParent();
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
  {
    std::lock_guard<std::mutex> lock(mErrorMutex);
    mError = error;
  }
  mQueue->wake(EventQueue::WAKE_ERROR);
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
  std::vector<std::wstring> newExcludedPaths(excludedPaths);
  std::lock_guard<std::mutex> lock(mExcludedPathsMutex);
  mExcludedPaths.swap(newExcludedPaths);
}
