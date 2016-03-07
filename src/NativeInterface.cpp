#include "../includes/NativeInterface.h"

#if defined(__APPLE_CC__) || defined(BSD)
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
