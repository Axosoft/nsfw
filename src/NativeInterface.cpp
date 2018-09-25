#include "nsfw/NativeInterface.h"
using namespace NSFW;

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

std::vector<Event*> *NativeInterface::getEvents() {
  return mQueue->dequeueAll().release();
}

std::unique_ptr<std::vector<std::unique_ptr<Event>>> NativeInterface::getEventVector() {
  return mQueue->dequeueAllEventsInVector();
}

bool NativeInterface::hasErrored() {
  return mNativeInterface->hasErrored();
}

bool NativeInterface::isWatching() {
  return mNativeInterface->isWatching();
}
