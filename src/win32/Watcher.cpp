#include "../../includes/win32/Watcher.h"

#include <algorithm>
#include <cwctype>

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
// What a read of a recursive watch holds: a whole tree changes at once, a checkout say.
static constexpr std::size_t RECURSIVE_BUFFER_SIZE = 1024 * 1024;
// The largest buffer a read over the network takes, see the ReadDirectoryChangesW docs. Also what a watcher that isn't
// recursive reads into: it only watches a file, whose watcher recovers from an overflow on its own.
static constexpr std::size_t NETWORK_BUFFER_SIZE = 64 * 1024;
static constexpr DWORD SPLIT_READ_WAIT_MS = 10;
// How long flush() goes on while reads keep coming, and while a rename waits for its new name: well past the rename's
// deadline, which the timers' resolution, about 16 ms, can put off.
static constexpr DWORD FLUSH_READS_WAIT_MS = 3 * SPLIT_READ_WAIT_MS;
static constexpr DWORD FLUSH_RENAME_WAIT_MS = 20 * SPLIT_READ_WAIT_MS;

// What the entry itself is, see Event::kind: the attributes of a symlink or a junction are its own, and one that leads
// to a directory has the directory attribute. The system doesn't tell what a removed entry was.
static PathFilter::Kind reportedKindOf(DWORD attributes) {
  if (attributes == INVALID_FILE_ATTRIBUTES) {
    return PathFilter::Kind::UNKNOWN;
  }
  return (attributes & FILE_ATTRIBUTE_DIRECTORY) ? PathFilter::Kind::DIRECTORY : PathFilter::Kind::FILE;
}

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
    const std::vector<std::wstring> &excludedPaths, std::shared_ptr<PathFilter> filter, bool recursive)
  : mRunning(false),
  mPath(path),
  mRecursive(recursive),
  mQueue(queue),
  mExcludedPaths(excludedPaths),
  mFilter(filter),
  mBufferSize(recursive ? RECURSIVE_BUFFER_SIZE : NETWORK_BUFFER_SIZE)
{
  mDirectoryHandle = openDirectory(mPath);
  if (mDirectoryHandle == INVALID_HANDLE_VALUE) {
    setError("Failed to open directory");
  } else {
    ZeroMemory(&mOverlapped, sizeof(OVERLAPPED));
    mOverlapped.hEvent = this;
    mWatchedPath = getWatchedPathFromHandle();
    openParent();
    start();
  }
}

Watcher::~Watcher() {
  stop();
  countTestBufferBytes(0, mReadBuffer.capacity() + mWriteBuffer.capacity());
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

void Watcher::run() {
  while(mRunning) {
    // A split rename normally continues in the next read. The deadlines also bound the wait under unrelated APCs.
    const ULONGLONG nextDeadline = (std::min)(mRenameDeadline, mRemovalDeadline);
    if (nextDeadline != NO_DEADLINE) {
      const ULONGLONG now = GetTickCount64();
      if (now >= nextDeadline) {
        if (now >= mRenameDeadline) flushPendingRename();
        if (now >= mRemovalDeadline) releaseRemoval();
      } else {
        SleepEx(static_cast<DWORD>(nextDeadline - now), true);
      }
    } else if (!mPollWatchedPath) {
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

#ifdef NSFW_TEST_FAULTS_1
std::atomic<std::size_t> Watcher::sTestBufferBytes{0};

// GetEnvironmentVariableW, not _wgetenv: the specs set it through process.env after the CRT copied the environment.
Watcher::TestFault Watcher::readTestFault() {
  wchar_t value[32];
  const DWORD length = GetEnvironmentVariableW(L"NSFW_TEST_READ_FAULT", value, 32);
  const std::wstring fault = length > 0 && length < 32 ? std::wstring(value, length) : std::wstring();
  if (fault == L"network-sync") {
    return TestFault::NETWORK_SYNC;
  } else if (fault == L"network-async") {
    return TestFault::NETWORK_ASYNC;
  } else if (fault == L"later-async") {
    return TestFault::LATER_ASYNC;
  }
  return TestFault::NONE;
}

// True when the read fails as mTestFault says, with the error the call gives.
bool Watcher::injectReadFault(DWORD &error) {
  const bool fails = mTestFault == TestFault::LATER_ASYNC
    ? mTestReads++ > 0
    : mTestFault != TestFault::NONE && mBufferSize > NETWORK_BUFFER_SIZE;
  if (!fails) {
    return false;
  }
  if (mTestFault == TestFault::NETWORK_SYNC) {
    error = ERROR_INVALID_PARAMETER;
    return true;
  }
  // On this thread, as a completion routine would run.
  QueueUserAPC([](__in ULONG_PTR self) {
    reinterpret_cast<Watcher*>(self)->eventCallback(ERROR_INVALID_PARAMETER, 0);
  }, GetCurrentThread(), (ULONG_PTR)this);
  error = ERROR_SUCCESS;
  return true;
}
#endif

// A share rejects a larger buffer with ERROR_INVALID_PARAMETER, either at once or when the read completes. The next
// reads take one of NETWORK_BUFFER_SIZE instead, on a handle opened again: the system may keep the size of the first
// read on a handle. False, with nothing done, for another error or when the buffer is that small already: the failure
// has another cause.
bool Watcher::retryWithNetworkBuffer(DWORD error) {
  if (error != ERROR_INVALID_PARAMETER || mBufferSize <= NETWORK_BUFFER_SIZE) {
    return false;
  }
  mBufferSize = NETWORK_BUFFER_SIZE;
  reopenWathedFolder();
  return true;
}

bool Watcher::pollDirectoryChanges() {
  if (!isRunning()) {
    return false;
  }

  const DWORD error = readDirectoryChanges();
  if (retryWithNetworkBuffer(error)) {
    return !hasError();
  }
  if (error != ERROR_SUCCESS) {
    setError(describeError("Service shutdown unexpectedly", error));
    return false;
  }
  return true;
}

// Assigned, not resized: resizing keeps the capacity.
void Watcher::allocateWriteBuffer() {
  const std::size_t freed = mWriteBuffer.capacity();
  mWriteBuffer = std::vector<BYTE>(mBufferSize);
  countTestBufferBytes(mWriteBuffer.capacity(), freed);
}

// Issues the next read, into the write buffer, which no read holds then. The read buffer may hold the batch being
// handled: it takes the size of the next reads when it becomes the write buffer, so a buffer allocated before a shrink
// is freed after the next read, and a watcher that has read nothing yet holds a single buffer.
DWORD Watcher::readDirectoryChanges() {
  if (mWriteBuffer.size() != mBufferSize) {
    allocateWriteBuffer();
  }
  DWORD bytes = 0;

  std::lock_guard<std::mutex> lock(mHandleMutex);
#ifdef NSFW_TEST_FAULTS_1
  DWORD fault;
  if (injectReadFault(fault)) {
    return fault;
  }
#endif
  if (!ReadDirectoryChangesW(
    mDirectoryHandle,
    mWriteBuffer.data(),
    static_cast<DWORD>(mWriteBuffer.size()),
    mRecursive ? TRUE : FALSE,
    FILE_NOTIFY_CHANGE_FILE_NAME
    | FILE_NOTIFY_CHANGE_DIR_NAME
    | FILE_NOTIFY_CHANGE_LAST_WRITE
    // A change of a watched file's attributes alone moves its change time, so it is a modification of the file.
    | (mRecursive ? 0 : FILE_NOTIFY_CHANGE_ATTRIBUTES),
    &bytes,                         // num bytes written
    &mOverlapped,
    [](DWORD errorCode, DWORD numBytes, LPOVERLAPPED overlapped) {
      auto watcher = reinterpret_cast<Watcher*>(overlapped->hEvent);
      watcher->eventCallback(errorCode, numBytes);
    }))
  {
    return GetLastError();
  }
  return ERROR_SUCCESS;
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
    // What this read lost may be the new name of the pending rename, or the old name of another one whose new name
    // starts the next read: the pending old name can't be paired anymore. It may also be the removal of an entry a
    // scan found, whose creation that follows would be taken for the one the scan already reported.
    flushPendingRename();
    releaseRemoval();
    mScannedEntries.clear();
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
    } else if (retryWithNetworkBuffer(errorCode)) {
      // Read again.
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
  ++mReads;
  // Not under a path that leads elsewhere now: the run ends with the error, and the batch with it.
  checkWatchedPath();
  if (hasError()) {
    // The run ends: an old name left waiting is not reported once its error is.
    mPendingOldName.clear();
    mRenameDeadline = NO_DEADLINE;
    return;
  }
  handleEvents();
}

static bool isSeparator(wchar_t character) {
  return character == L'\\' || character == L'/';
}

bool Watcher::isExcluded(const std::wstring &fileName) {
  std::wstring::size_type found = fileName.rfind('\\');
  return isExcludedPath(joinPath(mPath, found != std::wstring::npos ? fileName.substr(0, found) : fileName));
}

bool Watcher::isExcludedPath(const std::wstring &path) {
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
  forgetOldScans();
  if (!mRecursive) mQueue->holdEvents();
  BYTE *base = mReadBuffer.data();
  while (true) {
    PFILE_NOTIFY_INFORMATION info = (PFILE_NOTIFY_INFORMATION)base;
    std::wstring fileName = getWStringFileName(info->FileName, info->FileNameLength);

    if (info->Action == FILE_ACTION_RENAMED_NEW_NAME && !mPendingOldName.empty()) {
      handleRename(mPendingOldName, fileName);
      mPendingOldName.clear();
      mRenameDeadline = NO_DEADLINE;
    } else {
      flushPendingRename();
      switch (info->Action) {
      case FILE_ACTION_RENAMED_OLD_NAME:
        // Its new name follows, in this read or, when the old one ended it, at the start of the next one.
        mPendingOldName = fileName;
        break;
      case FILE_ACTION_ADDED:
        if (!consumeScanned(fileName)) {
          handleArrival(fileName);
        }
        break;
      case FILE_ACTION_RENAMED_NEW_NAME:
        forgetScanned(fileName);
        handleArrival(fileName);
        break;
      case FILE_ACTION_REMOVED:
        handleRemoval(fileName);
        break;
      case FILE_ACTION_MODIFIED:
      default:
        if (!isExcluded(fileName)) {
          mQueue->enqueue(MODIFIED, getUTF8Directory(fileName), getUTF8FileName(fileName), "", "",
            PathFilter::Kind::UNKNOWN, reportedKindOf(GetFileAttributesW(joinPath(mPath, fileName).data())));
        }
      };
    }

    if (info->NextEntryOffset == 0) {
      break;
    }
    base += info->NextEntryOffset;
  }
  const ULONGLONG now = GetTickCount64();
  if (!mPendingOldName.empty()) mRenameDeadline = now + SPLIT_READ_WAIT_MS;
  if (!mRecursive) {
    // Sibling notifications can keep reads coming without adding any events for the watched file.
    if (!mQueue->releaseEvents(now < mRemovalDeadline)) {
      mRemovalDeadline = NO_DEADLINE;
    } else if (mRemovalDeadline == NO_DEADLINE) {
      mRemovalDeadline = now + SPLIT_READ_WAIT_MS;
    }
  }
}

static std::wstring scannedKey(const std::wstring &fileName) {
  std::wstring key = fileName;
  if (!key.empty()) {
    CharLowerBuffW(&key[0], static_cast<DWORD>(key.size()));
  }
  return key;
}

namespace {
  struct EntryInfo {
    DWORD attributes;
    // Only meaningful when the attributes have FILE_ATTRIBUTE_REPARSE_POINT.
    DWORD reparseTag;
  };
}

static EntryInfo entryInfo(const WIN32_FIND_DATAW &data) {
  return { data.dwFileAttributes, data.dwReserved0 };
}

// GetFileAttributesW doesn't tell the reparse tag, which tells a link from a directory that is a placeholder of a
// cloud provider, whose entries the system does watch.
static EntryInfo entryInfo(const std::wstring &path) {
  const DWORD attributes = GetFileAttributesW(path.data());
  if (attributes == INVALID_FILE_ATTRIBUTES || !(attributes & FILE_ATTRIBUTE_REPARSE_POINT)) {
    return { attributes, 0 };
  }
  WIN32_FIND_DATAW data;
  HANDLE find = FindFirstFileW(path.data(), &data);
  if (find == INVALID_HANDLE_VALUE) {
    return { INVALID_FILE_ATTRIBUTES, 0 };
  }
  FindClose(find);
  return entryInfo(data);
}

static bool isLink(const EntryInfo &info) {
  return (info.attributes & FILE_ATTRIBUTE_REPARSE_POINT) && IsReparseTagNameSurrogate(info.reparseTag);
}

static PathFilter::Kind kindOf(const EntryInfo &info) {
  // The filter follows a symlink to tell what it leads to.
  if (info.attributes == INVALID_FILE_ATTRIBUTES || isLink(info)) {
    return PathFilter::Kind::UNKNOWN;
  }
  return (info.attributes & FILE_ATTRIBUTE_DIRECTORY) ? PathFilter::Kind::DIRECTORY : PathFilter::Kind::FILE;
}

// The system watches what a directory holds, not what a symlink or a junction in it leads to. Listing a directory
// whose entries a cloud provider hasn't brought locally would have it fetch them, on the watcher thread.
static bool holdsWatchedEntries(const EntryInfo &info) {
  return info.attributes != INVALID_FILE_ATTRIBUTES && (info.attributes & FILE_ATTRIBUTE_DIRECTORY) && !isLink(info) &&
    !(info.attributes & (FILE_ATTRIBUTE_RECALL_ON_OPEN | FILE_ATTRIBUTE_RECALL_ON_DATA_ACCESS));
}

void Watcher::handleArrival(const std::wstring &fileName) {
  if (isExcluded(fileName)) {
    return;
  }

  const EntryInfo info = entryInfo(joinPath(mPath, fileName));
  mQueue->enqueue(CREATED, getUTF8Directory(fileName), getUTF8FileName(fileName), "", "", kindOf(info),
    reportedKindOf(info.attributes));
  // The system reports a directory moved in, not what it holds: report that too, as on Linux. A copy has the creation
  // of each entry reported as well, which the scanned entries keep from being reported twice. A watch of the
  // directory's own entries has nothing below them to report.
  if (mRecursive && holdsWatchedEntries(info)) {
    reportContents(fileName);
  }
}

bool Watcher::isIgnoredDirectory(const std::wstring &fileName) {
  if (isExcludedPath(joinPath(mPath, fileName))) {
    return true;
  }
  return mFilter && mFilter->isDirectoryIgnored(getUTF8Directory(fileName), getUTF8FileName(fileName));
}

void Watcher::reportContents(const std::wstring &directory) {
  std::vector<std::wstring> directories(1, directory);
  // A large tree would otherwise hold stop() up until all of it is scanned.
  while (!directories.empty() && isRunning()) {
    const std::wstring relative = std::move(directories.back());
    directories.pop_back();
    // Asked once the entries next to it are reported, so that an ignore file among them applies.
    if (isIgnoredDirectory(relative)) {
      continue;
    }
    // The parent's watch can't tell of a rename of the watched directory while the scan holds this thread, and the
    // path listed would then lead elsewhere.
    checkWatchedPath();
    // Nor is a scan whose events the overflowed queue only drops worth finishing before the run ends.
    if (hasError() || mQueue->overflowed()) {
      return;
    }

    WIN32_FIND_DATAW data;
    HANDLE find = FindFirstFileW((joinPath(mPath, relative) + L"\\*").data(), &data);
    if (find == INVALID_HANDLE_VALUE) {
      continue;
    }
    do {
      const std::wstring name = data.cFileName;
      if (name == L"." || name == L"..") {
        continue;
      }
      const std::wstring entry = relative + L"\\" + name;
      mScannedEntries[scannedKey(entry)] = mReads;
      const EntryInfo info = entryInfo(data);
      mQueue->enqueue(CREATED, getUTF8Directory(entry), getUTF8FileName(entry), "", "", kindOf(info),
        reportedKindOf(info.attributes));
      if (holdsWatchedEntries(info)) {
        directories.push_back(entry);
      }
    } while (FindNextFileW(find, &data));
    FindClose(find);
  }
}

// A short name has '~' and a digit in one of its components; matching more names than that only costs duplicates.
static bool mayHoldShortName(const std::wstring &fileName) {
  for (size_t tilde = fileName.find(L'~'); tilde != std::wstring::npos; tilde = fileName.find(L'~', tilde + 1)) {
    if (tilde + 1 < fileName.size() && iswdigit(fileName[tilde + 1])) {
      return true;
    }
  }
  return false;
}

// Only a creation can be the late report of an entry a scan found: an entry renamed there replaces it, and is reported
// as the system tells.
bool Watcher::consumeScanned(const std::wstring &fileName) {
  return !mScannedEntries.empty() && mScannedEntries.erase(scannedKey(fileName)) != 0;
}

void Watcher::forgetScanned(const std::wstring &fileName) {
  if (mScannedEntries.empty()) {
    return;
  }
  // The system may name an entry by its short name, which the long names the scans found can't be matched with: a
  // later creation of what it named must not be taken for the one a scan reported.
  if (mayHoldShortName(fileName)) {
    mScannedEntries.clear();
    return;
  }
  const std::wstring key = scannedKey(fileName);
  mScannedEntries.erase(key);
  // ']' is the character after the backslash, so the keys below this one sort between these bounds.
  mScannedEntries.erase(mScannedEntries.lower_bound(key + L"\\"), mScannedEntries.lower_bound(key + L"]"));
}

void Watcher::forgetOldScans() {
  // The system recorded the creation of what a scan found before the scan ended, so its event comes at the latest in
  // the read issued after the one pending during the scan. A creation read after those two is a new one.
  for (auto entry = mScannedEntries.begin(); entry != mScannedEntries.end();) {
    if (entry->second + 2 < mReads) {
      entry = mScannedEntries.erase(entry);
    } else {
      ++entry;
    }
  }
}

void Watcher::handleRemoval(const std::wstring &fileName, PathFilter::Kind kind) {
  forgetScanned(fileName);
  if (!isExcluded(fileName)) {
    mQueue->enqueue(DELETED, getUTF8Directory(fileName), getUTF8FileName(fileName), "", "", PathFilter::Kind::UNKNOWN,
      kind);
  }
}

void Watcher::handleRename(const std::wstring &oldFileName, const std::wstring &newFileName) {
  forgetScanned(oldFileName);
  forgetScanned(newFileName);
  const bool oldExcluded = isExcluded(oldFileName);
  const bool newExcluded = isExcluded(newFileName);
  if (oldExcluded) {
    handleArrival(newFileName);
  } else if (newExcluded) {
    // Moved, not removed: what arrived can still be looked up, as when the filter turns a rename into a removal.
    handleRemoval(oldFileName, reportedKindOf(GetFileAttributesW(joinPath(mPath, newFileName).data())));
  } else {
    // The listener never saw what a directory held under an excluded or ignored name, whatever the queue makes of
    // the rename. The queue is told it is a directory, so that it judges the old name as such too.
    const EntryInfo info = entryInfo(joinPath(mPath, newFileName));
    const bool isDirectory = kindOf(info) == PathFilter::Kind::DIRECTORY;
    const bool fromIgnoredDirectory = mRecursive && holdsWatchedEntries(info) && isIgnoredDirectory(oldFileName);
    mQueue->enqueue(
      RENAMED,
      getUTF8Directory(oldFileName),
      getUTF8FileName(oldFileName),
      getUTF8Directory(newFileName),
      getUTF8FileName(newFileName),
      isDirectory ? PathFilter::Kind::DIRECTORY : PathFilter::Kind::UNKNOWN,
      reportedKindOf(info.attributes)
    );
    if (fromIgnoredDirectory) {
      reportContents(newFileName);
    }
  }
}

void Watcher::flushPendingRename() {
  if (!mPendingOldName.empty()) {
    handleRemoval(mPendingOldName);
    mPendingOldName.clear();
    mRenameDeadline = NO_DEADLINE;
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
  // The queue outlives the watcher.
  releaseRemoval();
}

// What the watcher thread has handled, sampled there: the reads it took in, and whether a rename waits for its new
// name. False if the thread ended first.
bool Watcher::sampleHandled(uint64_t &reads, bool &pendingRename) {
  struct Sample {
    Watcher *watcher;
    SingleshotSemaphore done;
    uint64_t reads = 0;
    bool pendingRename = false;
  };
  auto sample = std::make_shared<Sample>();
  sample->watcher = this;
  // Shared with the APC, which never runs if the thread ends first.
  auto queued = new std::shared_ptr<Sample>(sample);
  if (!QueueUserAPC([](__in ULONG_PTR queued) {
    std::unique_ptr<std::shared_ptr<Sample>> sample(reinterpret_cast<std::shared_ptr<Sample>*>(queued));
    (*sample)->reads = (*sample)->watcher->mReads;
    (*sample)->pendingRename = !(*sample)->watcher->mPendingOldName.empty();
    (*sample)->done.signal();
  }, mRunner.native_handle(), (ULONG_PTR)queued)) {
    delete queued;
    return false;
  }
  if (!sample->done.waitWhile([this] { return isRunning(); })) {
    return false;
  }
  reads = sample->reads;
  pendingRename = sample->pendingRename;
  return true;
}

// The completion routine of a read that completed before this call is queued to the watcher thread ahead of a later
// APC, but only once the system runs it there, and the read it issues again completes at once with what changed while
// it handled the previous one: so APCs go until one finds no new read, and no rename waiting for its new name, which
// the thread reports as a removal at its deadline. Best effort: a change the system holds for want of a read issued,
// or one a share reports late, is not covered.
void Watcher::flush() {
  const ULONGLONG start = GetTickCount64();
  uint64_t lastReads;
  bool pendingRename;
  if (!isRunning() || !sampleHandled(lastReads, pendingRename)) {
    return;
  }
  while (GetTickCount64() < start + (pendingRename ? FLUSH_RENAME_WAIT_MS : FLUSH_READS_WAIT_MS)) {
    if (pendingRename) {
      Sleep(SPLIT_READ_WAIT_MS);
    }
    uint64_t reads;
    if (!sampleHandled(reads, pendingRename) || (reads == lastReads && !pendingRename)) {
      return;
    }
    lastReads = reads;
  }
}

void Watcher::releaseRemoval() {
  if (mRemovalDeadline != NO_DEADLINE) {
    mRemovalDeadline = NO_DEADLINE;
    mQueue->releaseEvents();
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
