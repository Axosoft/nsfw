#ifndef INOTIFY_EVENT_LOOP_H
#define INOTIFY_EVENT_LOOP_H

#include "InotifyService.h"
#include "../SingleshotSemaphore.h"

#include <sys/inotify.h>
#include <stdlib.h>
#include <string.h>
#include <pthread.h>
#include <unistd.h>
#include <sys/select.h>
#include <string>
#include <mutex>
#include <system_error>

class InotifyService;
class Lock;

class InotifyEventLoop {
public:
  InotifyEventLoop(
    int inotifyInstance,
    InotifyService *inotifyService
  );

  // Whether the thread was created. It stays true after the loop stops on its own, since that always records a tree
  // error, which is how the poll thread finds out (also when the loop stops before start() checks isWatching()).
  bool isStarted();

  // Holds off event handling while the returned lock lives, so another thread can change the tree the handlers use.
  std::unique_lock<std::mutex> lockEventHandling();

  void work();

  ~InotifyEventLoop();
private:
  static const int BUFFER_SIZE = 8192;
  struct InotifyRenameEvent {
    uint32_t cookie;
    bool isDirectory;
    bool isStarted;
    std::string name;
    int wd;
  };

  int mInotifyInstance;
  InotifyService *mInotifyService;

  pthread_t mEventLoop;
  std::mutex mMutex;
  SingleshotSemaphore mLoopingSemaphore;
  // Only the constructor writes it. The destructor joins the thread whenever it was created.
  bool mStarted;
};

#endif
