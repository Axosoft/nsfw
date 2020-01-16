#include "../includes/win32/Controller.h"

static std::wstring convertMultiByteToWideCharNTPath(const std::string &multiByte) {
  const int wlen = MultiByteToWideChar(CP_UTF8, 0, multiByte.data(), -1, 0, 0);

  if (wlen == 0) {
    return std::wstring();
  }

  std::wstring wideString;
  wideString.resize(wlen - 1);

  int failureToResolveUTF8 = MultiByteToWideChar(CP_UTF8, 0, multiByte.data(), -1, &(wideString[0]), wlen);
  if (failureToResolveUTF8 == 0) {
    return std::wstring();
  }

  if (wideString.rfind(L"\\\\?\\", 0) == 0 || wideString.rfind(L"\\??\\", 0) == 0) {
    // We already have an NT Path
    return wideString;
  }

  const ULONG widePathLength = GetFullPathNameW(wideString.c_str(), 0, nullptr, nullptr);
  if (widePathLength == 0) {
    return wideString;
  }

  std::wstring pathString;
  pathString.resize(widePathLength - 1);

  if (GetFullPathNameW(wideString.c_str(), widePathLength, &(pathString[0]), nullptr) != widePathLength - 1) {
    // Failed to get full path name
    return wideString;
  }

  // We return an NT Path to support paths > MAX_PATH
  return pathString.rfind(L"\\\\", 0) == 0
    ? pathString.replace(0, 2, L"\\\\?\\UNC\\")
    : pathString.replace(0, 0, L"\\\\?\\");
}

HANDLE Controller::openDirectory(const std::wstring &path) {
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

Controller::Controller(std::shared_ptr<EventQueue> queue, const std::string &path)
  : mDirectoryHandle(INVALID_HANDLE_VALUE)
{
  auto widePath = convertMultiByteToWideCharNTPath(path);
  mDirectoryHandle = openDirectory(widePath);

  if (mDirectoryHandle == INVALID_HANDLE_VALUE) {
    return;
  }

  mWatcher.reset(new Watcher(queue, mDirectoryHandle, widePath));
}

Controller::~Controller() {
  mWatcher.reset();
  CancelIo(mDirectoryHandle);
  CloseHandle(mDirectoryHandle);
  mDirectoryHandle = INVALID_HANDLE_VALUE;
}

std::string Controller::getError() {
  if (mDirectoryHandle == INVALID_HANDLE_VALUE) {
    return "Failed to open directory";
  }
  return mWatcher->getError();
}

bool Controller::hasErrored() {
  return mDirectoryHandle == INVALID_HANDLE_VALUE || !mWatcher->getError().empty();
}

bool Controller::isWatching() {
  return !hasErrored() && mWatcher->isRunning();
}
