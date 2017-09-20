#include <utility>
#include <chrono>
#include <memory>
#include <thread>
#include <atomic>

#include <iostream>

#include "Listener.hpp"
#include "NativeInterface.h"

namespace NodeSentinalFileWatcher {

using CallBackSignatur = std::function<void(std::unique_ptr<std::vector<std::unique_ptr<Event>>>)>;

class FileSystemWatcher : public Listener<CallBackSignatur>
{
  std::unique_ptr<NativeInterface> _nativeInterface;
  Listener::CallbackHandle _callbackHandle;
  std::chrono::milliseconds _sleepDuration;
  std::thread _runner;
  std::atomic<bool> _inDestruction{false};

public:
  FileSystemWatcher(const std::string &path, std::chrono::milliseconds sleepDuration);
  ~FileSystemWatcher();
  void onChanges(CallBackSignatur callBack);
};

}
