#ifndef INOTIFY_EVENT_LOOP_H
#define INOTIFY_EVENT_LOOP_H

#include "InotifyService.h"
#include "../Lock.h"
#include <sys/inotify.h>
#include <stdlib.h>
#include <string.h>
#include <pthread.h>
#include <unistd.h>
#include <sys/select.h>
#include <string>

class InotifyService;
class Lock;

class InotifyEventLoop {
public:
  InotifyEventLoop(
    int inotifyInstance,
    InotifyService *inotifyService
  );

  bool isLooping();

  void work();

  ~InotifyEventLoop();
private:
  static const int BUFFER_SIZE = 8192;
  struct InotifyRenameEvent {
    uint32_t cookie;
    bool isDirectory;
    bool isGood;
    std::string name;
    int wd;
  };

  int mInotifyInstance;
  InotifyService *mInotifyService;

  pthread_t mEventLoop;
  pthread_mutex_t mMutex;
  bool mStarted;
};

#endif
