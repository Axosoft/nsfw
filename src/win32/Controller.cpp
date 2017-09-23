#include "../includes/win32/Controller.h"

static std::wstring convertMultiByteToWideChar(const std::string &multiByte) {
  const int wlen = MultiByteToWideChar(CP_UTF8, 0, multiByte.data(), -1, 0, 0);

  if (wlen == 0) {
    return std::wstring();
  }

  std::wstring wideString;
  wideString.resize(wlen-1);
  int failureToResolveUTF8 = MultiByteToWideChar(CP_UTF8, 0, multiByte.data(), -1, &(wideString[0]), wlen);
  if (failureToResolveUTF8 == 0) {
    return std::wstring();
  }
  return wideString;
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
  auto widePath = convertMultiByteToWideChar(path);
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
  return mWatcher->isRunning();
}
