#include "../includes/NativeInterface.h"

NativeInterface::NativeInterface(const std::string &path, std::shared_ptr<EventQueue> queue, std::function<bool(std::string path)> ignorePath) {
  mNativeInterface.reset(new NativeImplementation(queue, path, ignorePath));
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
