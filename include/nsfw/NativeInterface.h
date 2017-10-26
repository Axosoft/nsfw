#ifndef NSFW_NATIVE_INTERFACE_H
#define NSFW_NATIVE_INTERFACE_H

#if defined(_WIN32)
#include "nsfw/win32/Controller.h"
using NativeImplementation = Controller;
#elif defined(__APPLE_CC__)
#include "nsfw/osx/FSEventsService.h"
using NativeImplementation = FSEventsService;
#elif defined(__linux__) || defined(__FreeBSD__)
#include "nsfw/linux/InotifyService.h"
using NativeImplementation = InotifyService;
#endif

#include "Queue.h"
#include <vector>

class NativeInterface {
public:
  NativeInterface(const std::string &path);
  ~NativeInterface();

  std::string getError();

  std::vector<Event*> *getEvents();
  std::unique_ptr<std::vector<std::unique_ptr<Event>>> getEventVector();

  bool hasErrored();
  bool isWatching();

private:
  std::shared_ptr<EventQueue> mQueue;
  std::unique_ptr<NativeImplementation> mNativeInterface;
};

#endif
