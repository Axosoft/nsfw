#ifndef NSFW_NATIVE_INTERFACE_H
#define NSFW_NATIVE_INTERFACE_H

#if defined(_WIN32)
#include "../includes/win32/Controller.h"
using NativeImplementation = Controller;
#elif defined(__APPLE_CC__)
#include "../includes/osx/FSEventsService.h"
using NativeImplementation = FSEventsService;
#elif defined(__linux__) || defined(__FreeBSD__)
#include "../includes/linux/InotifyService.h"
using NativeImplementation = InotifyService;
#endif

#include "Queue.h"
#include <vector>

class NativeInterface {
public:
  NativeInterface(const std::string &path);
  ~NativeInterface();

  std::string getError();
  std::unique_ptr<std::vector<std::unique_ptr<Event>>> getEvents();
  bool hasErrored();
  bool isWatching();

private:
  std::shared_ptr<EventQueue> mQueue;
  std::unique_ptr<NativeImplementation> mNativeInterface;
};

#endif
