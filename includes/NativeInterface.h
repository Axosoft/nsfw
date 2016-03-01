#ifndef NSFW_NATIVE_INTERFACE_H
#define NSFW_NATIVE_INTERFACE_H

#include "linux/InotifyService.h"

class NativeInterface {
public:
  NativeInterface(std::string path);
  ~NativeInterface();
private:
  void *mNativeInterface;
};

#endif
