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
  return mStarted && !mExited;
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
  mExited = true;
}
