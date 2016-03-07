#include "../../includes/osx/RunLoop.h"

#include <iostream>

void *scheduleRunLoopWork(void *runLoop) {
  ((RunLoop *)runLoop)->work();
  return NULL;
}

RunLoop::RunLoop(FSEventsService *eventsService, std::string path):
  mEventsService(eventsService),
  mPath(path),
  mRunLoop(NULL),
  mStarted(false) {
  if (pthread_mutex_init(&mMutex, NULL) != 0) {
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
  return mStarted;
}

RunLoop::~RunLoop() {
  if (!mStarted) {
    return;
  }

  FSEventStreamStop(mEventStream);
  FSEventStreamInvalidate(mEventStream);
  FSEventStreamRelease(mEventStream);

  while(!CFRunLoopIsWaiting(mRunLoop)) {}
  CFRunLoopStop(mRunLoop);

  {
    Lock syncWithWork(this->mMutex);
    pthread_cancel(mRunLoopThread);
  }

  pthread_join(mRunLoopThread, NULL);
  pthread_mutex_destroy(&mMutex);
}

void RunLoop::work() {
  Lock syncWithDestructor(this->mMutex);

  CFAbsoluteTime latency = 1;
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
  FSEventStreamContext callbackInfo;
  callbackInfo.version = 0;
  callbackInfo.info = (void *)mEventsService;

  mRunLoop = CFRunLoopGetCurrent();
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
}
