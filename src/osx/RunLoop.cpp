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
  if (uv_sem_init(&mReadyForCleanup, 0) != 0) {
    mStarted = false;
    return;
  }

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

  uv_sem_wait(&mReadyForCleanup);

  CFRunLoopStop(mRunLoop);

  pthread_join(mRunLoopThread, NULL);
  uv_sem_destroy(&mReadyForCleanup);
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

  __block uv_sem_t *runLoopHasStarted = &mReadyForCleanup;
  CFRunLoopPerformBlock(mRunLoop, kCFRunLoopDefaultMode, ^ {
    uv_sem_post(runLoopHasStarted);
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
