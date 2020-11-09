#include "../includes/NativeInterface.h"

NativeInterface::NativeInterface(const std::string &path, std::shared_ptr<EventQueue> queue, std::shared_ptr<PathFilter> pathFilter) {
  mNativeInterface.reset(new NativeImplementation(queue, path, pathFilter));
}

NativeInterface::~NativeInterface() {
  mNativeInterface.reset();
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
