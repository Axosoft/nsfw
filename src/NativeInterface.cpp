#include "../includes/NativeInterface.h"

#if defined(_WIN32)
#define SERVICE FSWatcherWrapper
#include "../includes/win32/FSWatcherWrapper.h"
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
