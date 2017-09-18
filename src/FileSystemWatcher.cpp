#include "../includes/FileSystemWatcher.h"

using namespace NodeSentinalFileWatcher;

FileSystemWatcher::FileSystemWatcher(const std::string &path, std::chrono::milliseconds sleepDuration)
    : _nativeInterface(new NativeInterface(path))
    , _sleepDuration(sleepDuration)
{
  _runner = std::thread([this] {
    while(_nativeInterface->isWatching()) {
       if (_nativeInterface->hasErrored()) {
         break;
       }

       auto events = _nativeInterface->getEventVector();
       if (events == nullptr) {
         std::this_thread::sleep_for(_sleepDuration);
         continue;
       }

       notify(events);

       std::this_thread::sleep_for(_sleepDuration);
    }
  });
}

FileSystemWatcher::~FileSystemWatcher()
{
  deregisterCallback(_callbackHandle);
  _runner.join();
}

void FileSystemWatcher::onChanges(CallBackSignatur callBack)
{
  _callbackHandle = registerCallback(callBack);
}
