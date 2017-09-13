#include "../includes/NativeInterface.h"

NativeInterface::NativeInterface(const std::string &path) {
  mQueue = std::make_shared<EventQueue>();
  mNativeInterface.reset(new NativeImplementation(mQueue, path));
}

NativeInterface::~NativeInterface() {
  mNativeInterface.reset();
}

std::string NativeInterface::getError() {
  return mNativeInterface->getError();
}

std::vector<Event *>* NativeInterface::getEvents() {
  return mQueue->dequeueAll().release();
}

bool NativeInterface::hasErrored() {
  return mNativeInterface->hasErrored();
}

bool NativeInterface::isWatching() {
  return mNativeInterface->isWatching();
}
