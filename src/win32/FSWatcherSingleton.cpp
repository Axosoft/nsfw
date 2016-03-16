#include "../../includes/win32/FSWatcherSingleton.h"

#pragma managed
FSWatcherSingleton::FSWatcherSingleton() {
  mWatcherMap = gcnew Dictionary<Int32, FSWatcher^>();
  mNext = 0;
}

int FSWatcherSingleton::createFileWatcher(EventQueue &queue, System::String ^path) {
  try {
    FSWatcher ^watcher = gcnew FSWatcher(queue, path);
    int wd = mNext;
    mNext = mNext == Int32::MaxValue ? 0 : mNext + 1;
    mWatcherMap[wd] = watcher;
    return wd;
  } catch (System::Exception ^e) {
    return -1;
  }
}

void FSWatcherSingleton::destroyFileWatcher(Int32 wd) {
  if (!mWatcherMap->ContainsKey(wd)) {
    return;
  }

  FSWatcher ^watcher = mWatcherMap[wd];
  mWatcherMap->Remove(wd);
  delete watcher;
}

bool FSWatcherSingleton::didFileWatcherError(Int32 wd) {
  if (!mWatcherMap->ContainsKey(wd)) {
    return true;
  }

  return mWatcherMap[wd]->hasErrored();
}

System::String ^FSWatcherSingleton::getFileWatcherError(int wd) {
  if (!mWatcherMap->ContainsKey(wd)) {
    return "";
  }

  return mWatcherMap[wd]->getError();
}

int createFileWatcher(EventQueue &queue, std::string path) {
  array<Byte> ^buffer = gcnew array<Byte>(path.length());
  System::Text::Encoding ^encoding = System::Text::Encoding::UTF8;
  for (int i = 0; i < path.length(); ++i) {
    buffer[i] = path[i];
  }
  return FSWatcherSingleton::Instance->createFileWatcher(queue, encoding->GetString(buffer));
}

void destroyFileWatcher(int wd) {
  FSWatcherSingleton::Instance->destroyFileWatcher(wd);
}

bool didFileWatcherError(int wd) {
  return FSWatcherSingleton::Instance->didFileWatcherError(wd);
}

std::string getFileWatcherError(int wd) {
  char *ptr = (char *)Marshal::StringToHGlobalAnsi(
    FSWatcherSingleton::Instance->getFileWatcherError(wd)
  ).ToPointer();
  std::string error = ptr;
  Marshal::FreeHGlobal(IntPtr(ptr));
  return error;
}
