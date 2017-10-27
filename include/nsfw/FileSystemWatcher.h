#ifndef NSFW_FILESYSTEMWATCHER_H
#define NSFW_FILESYSTEMWATCHER_H

#include <utility>
#include <chrono>
#include <memory>
#include <thread>
#include <atomic>

#include <iostream>

#include "Listener.hpp"
#include "NativeInterface.h"

namespace NSFW {

using CallBackSignatur = std::function<void(std::unique_ptr<std::vector<std::unique_ptr<Event>>>)>;

class FileSystemWatcher : public Listener<CallBackSignatur>
{
  std::unique_ptr<NativeInterface> mNativeInterface;
  Listener::CallbackHandle mCallbackHandle;
  std::chrono::milliseconds mSleepDuration;
  std::thread mRunner;
  std::atomic<bool> mInDestruction{false};

public:
  FileSystemWatcher(const std::string &path, std::chrono::milliseconds sleepDuration);
  ~FileSystemWatcher();
  void onChanges(CallBackSignatur callBack);
};

}

#endif //NSFW_FILESYSTEMWATCHER_H
