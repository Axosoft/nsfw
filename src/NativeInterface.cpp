#include "../includes/NativeInterface.h"

NativeInterface::NativeInterface(std::string path) {
  mNativeInterface.reset(new NativeImplementation(mQueue, path));
}

std::string NativeInterface::getError() {
  return mNativeInterface->getError();
}

std::vector<Event *>* NativeInterface::getEvents() {
  return mQueue.dequeueAll().release();
}

bool NativeInterface::hasErrored() {
  return mNativeInterface->hasErrored();
}

bool NativeInterface::isWatching() {
  return mNativeInterface->isWatching();
}
