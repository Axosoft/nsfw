#include "../../includes/osx/RunLoop.h"

void *scheduleRunLoopWork(void *runLoop) {
  ((RunLoop *)runLoop)->work();
  return NULL;
}

RunLoop::RunLoop(FSEventsService *eventsService, std::string path):
  mEventsService(eventsService),
  mExited(false),
  mPath(path),
  mRunLoop(NULL),
  mStarted(false) {
  mStarted = !pthread_create(
    &mRunLoopThread,
    NULL,
    scheduleRunLoopWork,
    (void *)this
  );
}

bool RunLoop::isLooping() {
  return mStarted && !mExited;
}

RunLoop::~RunLoop() {
  if (!mStarted) {
    return;
  }

  std::unique_lock<std::mutex> lk(mReadyForCleanupMutex);
  mReadyForCleanup.wait(lk);

  CFRunLoopStop(mRunLoop);

  pthread_join(mRunLoopThread, NULL);
}

void RunLoop::work() {
  CFAbsoluteTime latency = 0.001;
  CFStringRef fileWatchPath = CFStringCreateWithCString(
    NULL,
    mPath.c_str(),
    kCFStringEncodingUTF8
  );
  CFArrayRef pathsToWatch = CFArrayCreate(
    NULL,
    (const void **)&fileWatchPath,
    1,
    NULL
  );
  FSEventStreamContext callbackInfo {0, (void *)mEventsService, nullptr, nullptr, nullptr};

  mRunLoop = CFRunLoopGetCurrent();

  __block auto *runLoopHasStarted = &mReadyForCleanup;
  CFRunLoopPerformBlock(mRunLoop, kCFRunLoopDefaultMode, ^ {
    runLoopHasStarted->notify_all();
  });

  CFRunLoopWakeUp(mRunLoop);

  mEventStream = FSEventStreamCreate(
    NULL,
    &FSEventsServiceCallback,
    &callbackInfo,
    pathsToWatch,
    kFSEventStreamEventIdSinceNow,
    latency,
    kFSEventStreamCreateFlagFileEvents
  );

  FSEventStreamScheduleWithRunLoop(mEventStream, mRunLoop, kCFRunLoopDefaultMode);
  FSEventStreamStart(mEventStream);
  CFRelease(pathsToWatch);
  CFRelease(fileWatchPath);

  CFRunLoopRun();

  FSEventStreamStop(mEventStream);
  FSEventStreamUnscheduleFromRunLoop(mEventStream, mRunLoop, kCFRunLoopDefaultMode);
  FSEventStreamInvalidate(mEventStream);
  FSEventStreamRelease(mEventStream);

  mExited = true;
}
