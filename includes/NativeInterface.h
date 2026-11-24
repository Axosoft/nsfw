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

#include "PathFilter.h"
#include "Queue.h"
#include <vector>
#include <future>

class NativeInterface {
public:
  NativeInterface(const std::string &path, const std::vector<std::string> &excludedPaths,
    const FilterOptions &filterOptions, std::shared_ptr<EventQueue> queue);
  ~NativeInterface();

  std::string getError();
  bool hasErrored();
  bool isWatching();
  std::future<bool> updateExcludedPaths(const std::vector<std::string> &excludedPaths);
  void updateExcludedPatterns(const std::vector<std::string> &excludedPatterns);

private:
  std::shared_ptr<EventQueue> mQueue;
  std::shared_ptr<PathFilter> mFilter;
  std::unique_ptr<NativeImplementation> mNativeInterface;
};

#endif
