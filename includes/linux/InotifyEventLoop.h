#ifndef INOTIFY_EVENT_LOOP_H
#define INOTIFY_EVENT_LOOP_H

#include "InotifyService.h"
#include "InotifyTree.h"
#include "../SingleshotSemaphore.h"

#include <sys/inotify.h>
#include <stdlib.h>
#include <string.h>
#include <pthread.h>
#include <unistd.h>
#include <sys/select.h>
#include <chrono>
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
  // error, which is how the delivery thread finds out (also when the loop stops before start() checks isWatching()),
  // or follows an overflow of the event queue, which the queue tells the delivery thread itself.
  bool isStarted();

  // Holds off event handling while the returned lock lives, so another thread can change the tree the handlers use.
  std::unique_lock<std::mutex> lockEventHandling();

  void work();
  void wakeForEntryRetry();
  // Has the loop run its maintenance, see InotifyService::settleFilterChange(), without waiting for an event.
  void wake();
  // Handles every event the descriptor holds now, with event handling locked, so they are judged by the filter as it
  // is, and the move pending at the end, which the loop would only pair or give up on later. See
  // InotifyService::updateExcludedPatterns(). False when the run ended, before or meanwhile.
  bool drain();

#ifdef NSFW_TEST_SLOW_1
  static bool failReads(int inotifyInstance);
#endif

  ~InotifyEventLoop();

private:
  static const int BUFFER_SIZE = InotifyTree::READ_SIZE;
  // How long a move out of a watched directory whose other half didn't come in the same read waits for it, see work().
  static const int RENAME_PAIRING_TIMEOUT_MS = 10;
  // How many reads a drain takes at most when the size of the queue can't be told, see drain().
  static const int UNKNOWN_QUEUE_DRAIN_READS = 128;
  struct InotifyRenameEvent {
    uint32_t cookie;
    bool isDirectory;
    bool isStarted;
    std::string name;
    int wd;
    // When it is given up on, see InotifyEventLoop::giveUpOnExpiredRename(): a budget from its IN_MOVED_FROM, however
    // many reads come before its other half.
    std::chrono::steady_clock::time_point giveUpAt;
  };

  ssize_t readBuffer(char *buffer, std::string &error);
  bool handleBuffer(char *buffer, ssize_t bytesRead, int &eventsRead, std::string &error);
  void abandonRename();
  int renameWait();
  void giveUpOnExpiredRename();
  ssize_t readEvents(char *buffer, std::unique_lock<std::mutex> *loopLock = nullptr);
  void finishBatch(int eventsRead);
  int waitForEvents(int timeout);
  void endRun(const std::string &error);

#ifdef NSFW_TEST_SLOW_1
  void runTestHook(const inotify_event *event);
  void pauseAfterRead(std::unique_lock<std::mutex> &lock, const char *buffer, ssize_t bytesRead);
  void waitForTestFile(const std::string &directory, const std::string &release);
#endif

  int mInotifyInstance;
  InotifyService *mInotifyService;

  pthread_t mEventLoop;
  std::mutex mMutex;
  SingleshotSemaphore mLoopingSemaphore;
  // Only the constructor writes it. The destructor joins the thread whenever it was created.
  int mWakePipe[2] = { -1, -1 };
  bool mStarted = false;
  // With event handling locked: a move whose IN_MOVED_TO hasn't been read yet, and whether the run ended.
  InotifyRenameEvent mRename = {};
  bool mEnded = false;
};

#endif
