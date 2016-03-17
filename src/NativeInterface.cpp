#include "../includes/NativeInterface.h"

#if defined(_WIN32)
#define SERVICE ReadLoop
#include "../includes/win32/ReadLoop.h"
#elif defined(__APPLE_CC__) || defined(BSD)
#define SERVICE FSEventsService
#include "../includes/osx/FSEventsService.h"
#elif defined(__linux__)
#define SERVICE InotifyService
#include "../includes/linux/InotifyService.h"
#endif

NativeInterface::NativeInterface(std::string path) {
  mNativeInterface = new SERVICE(mQueue, path);
}

NativeInterface::~NativeInterface() {
  delete (SERVICE *)mNativeInterface;
}

std::string NativeInterface::getError() {
  return ((SERVICE *)mNativeInterface)->getError();
}

std::vector<Event *> *NativeInterface::getEvents() {
  if (mQueue.count() == 0) {
    return NULL;
  }

  int count = mQueue.count();
  std::vector<Event *> *events = new std::vector<Event *>;
  events->reserve(count);
  for (int i = 0; i < count; ++i) {
    events->push_back(mQueue.dequeue());
  }

  return events;
}

bool NativeInterface::hasErrored() {
  return ((SERVICE *)mNativeInterface)->hasErrored();
}

bool NativeInterface::isWatching() {
  return ((SERVICE *)mNativeInterface)->isWatching();
}
