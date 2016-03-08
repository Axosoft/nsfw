#ifndef NSFW_NATIVE_INTERFACE_H
#define NSFW_NATIVE_INTERFACE_H

#include "Queue.h"
#include <vector>

class NativeInterface {
public:
  NativeInterface(std::string path);

  std::vector<Event *> *getEvents();

  ~NativeInterface();
private:
  EventQueue mQueue;
  void *mNativeInterface;
};

#endif
