#include "nsfw/FileSystemWatcher.h"

using namespace NSFW;

FileSystemWatcher::FileSystemWatcher(const std::string &path, std::chrono::milliseconds sleepDuration)
    : mNativeInterface(new NativeInterface(path))
    , mSleepDuration(sleepDuration)
{
  mRunner = std::thread([this] {
    while(mNativeInterface->isWatching() && !mInDestruction) {
      if (mNativeInterface->hasErrored()) {
        break;
      }

      auto events = mNativeInterface->getEventVector();
      if (events != nullptr) {
        notify(events);
      }

      std::this_thread::sleep_for(mSleepDuration);
    }
  });
}

FileSystemWatcher::~FileSystemWatcher()
{
  deregisterCallback(mCallbackHandle);
  mInDestruction = true;
  mRunner.join();
}

void FileSystemWatcher::onChanges(CallBackSignatur callBack)
{
  mCallbackHandle = registerCallback(callBack);
}
