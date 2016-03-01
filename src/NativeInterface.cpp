#include "../includes/NativeInterface.h"

NativeInterface::NativeInterface(std::string path) {
  mNativeInterface = new InotifyService(path);
}

NativeInterface::~NativeInterface() {
  delete (InotifyService *)mNativeInterface;
}
