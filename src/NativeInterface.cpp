#include "../includes/NativeInterface.h"

NativeInterface::NativeInterface(const std::string &path, const std::vector<std::string> &excludedPaths, std::shared_ptr<EventQueue> queue) {
  mNativeInterface.reset(new NativeImplementation(queue, path, excludedPaths));
}

NativeInterface::~NativeInterface() {
  mNativeInterface.reset(nullptr);
}

std::string NativeInterface::getError() {
  return mNativeInterface->getError();
}

bool NativeInterface::hasErrored() {
  return mNativeInterface->hasErrored();
}

bool NativeInterface::isWatching() {
  return mNativeInterface->isWatching();
}

std::future<bool> NativeInterface::updateExcludedPaths(const std::vector<std::string> &excludedPaths) {
#if defined(__linux__) || defined(__FreeBSD__)
  return mNativeInterface->updateExcludedPaths(excludedPaths);
#else
  mNativeInterface->updateExcludedPaths(excludedPaths);
  return {};
#endif
}
