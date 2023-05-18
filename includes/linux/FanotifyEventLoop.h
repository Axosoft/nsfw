#ifndef FANOTIFY_EVENT_LOOP_H
#define FANOTIFY_EVENT_LOOP_H

#include "FanotifyService.h"
#include "../SingleshotSemaphore.h"

#include <sys/fanotify.h>
#include <stdlib.h>
#include <unistd.h>
#include <string>
#include <mutex>

class FanotifyService;
class Lock;

class FanotifyEventLoop {
public:
  FanotifyEventLoop(
    int fanotifyInstance,
    FanotifyService *inotifyService
  );

  bool isLooping();

  void work();

  ~FanotifyEventLoop();
private:
  static const int BUFFER_SIZE = 8192;
  struct FanotifyRenameEvent {
    bool isStarted;
    std::string name;
    int fd;
  };

  int mFanotifyInstance;
  FanotifyService *mFanotifyService;

  pthread_t mEventLoop;
  std::mutex mMutex;
  SingleshotSemaphore mLoopingSemaphore;
  bool mStarted;
};

#endif
