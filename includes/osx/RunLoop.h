#ifndef NSFW_RUNLOOP_H
#define NSFW_RUNLOOP_H

#include "../SingleshotSemaphore.h"
#include "FSEventsService.h"

#include <CoreServices/CoreServices.h>
#include <atomic>
#include <thread>
#include <string>

void *scheduleRunLoopWork(void *runLoop);
void FSEventsServiceCallback(
  ConstFSEventStreamRef streamRef,
  void *clientCallBackInfo,
  size_t numEvents,
  void *eventPaths,
  const FSEventStreamEventFlags eventFlags[],
  const FSEventStreamEventId eventIds[]
);

class FSEventsService;

class RunLoop {
public:
  // barrierDirectory: watched too, see FSEventsService::flush(); none if empty.
  RunLoop(FSEventsService *eventsService, std::string path, std::string barrierDirectory);

  friend void *scheduleRunLoopWork(void *runLoop);
  bool isLooping();
  // Has the run loop thread deliver what FSEvents holds for the stream, and waits until it did, see
  // FSEventStreamFlushSync(). Nothing once the run loop ended.
  void flush();

  ~RunLoop();
private:
  void work();

  FSEventsService *mEventsService;
  FSEventStreamRef mEventStream;
  std::atomic<bool> mExited;
  std::string mPath;
  std::string mBarrierDirectory;
  CFRunLoopRef mRunLoop;
  std::thread mRunLoopThread;
  SingleshotSemaphore mReadyForCleanup;
  SingleshotSemaphore mStreamStarted;
  bool mStarted;
};

#endif
