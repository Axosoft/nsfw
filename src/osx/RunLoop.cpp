#include "../../includes/osx/RunLoop.h"

#include <memory>

RunLoop::RunLoop(FSEventsService *eventsService, std::string path, std::string barrierDirectory):
  mEventsService(eventsService),
  mExited(false),
  mPath(path),
  mBarrierDirectory(barrierDirectory),
  mRunLoop(NULL),
  mStarted(false) {
  mRunLoopThread = std::thread([] (RunLoop *rl) { rl->work(); }, this);
  mStarted = mRunLoopThread.joinable();
  // The stream starts at kFSEventStreamEventIdSinceNow, so without this wait start() can resolve
  // before FSEventStreamStart and lose the caller's first changes.
  if (mStarted) {
    mStreamStarted.wait();
  }
}

bool RunLoop::isLooping() {
  return mStarted && !mExited;
}

void RunLoop::flush() {
  if (!isLooping()) {
    return;
  }
  // Shared with the block, which outlives this call if the run loop ends before running it.
  auto flushed = std::make_shared<SingleshotSemaphore>();
  FSEventStreamRef stream = mEventStream;
  CFRunLoopPerformBlock(mRunLoop, kCFRunLoopDefaultMode, ^ {
    FSEventStreamFlushSync(stream);
    flushed->signal();
  });
  CFRunLoopWakeUp(mRunLoop);
  flushed->waitWhile([this] { return isLooping(); });
}

RunLoop::~RunLoop() {
  if (!mStarted) {
    return;
  }

  mReadyForCleanup.wait();
  CFRunLoopStop(mRunLoop);

  mRunLoopThread.join();
  CFRelease(mRunLoop);
}

void RunLoop::work() {
  CFAbsoluteTime latency = 0.001;
  CFStringRef watchedPaths[] = {
    CFStringCreateWithCString(NULL, mPath.c_str(), kCFStringEncodingUTF8),
    mBarrierDirectory.empty() ? NULL : CFStringCreateWithCString(NULL, mBarrierDirectory.c_str(), kCFStringEncodingUTF8)
  };
  CFArrayRef pathsToWatch = CFArrayCreate(
    NULL,
    (const void **)watchedPaths,
    watchedPaths[1] == NULL ? 1 : 2,
    NULL
  );
  FSEventStreamContext callbackInfo {0, (void *)mEventsService, nullptr, nullptr, nullptr};

  // CFRunLoopGetCurrent doesn't retain, and the thread frees its run loop when it exits. CFRunLoopRun can return
  // on its own when the mode runs out of sources, so the destructor's CFRunLoopStop would touch a freed object.
  mRunLoop = (CFRunLoopRef)CFRetain(CFRunLoopGetCurrent());

  __block auto *runLoopHasStarted = &mReadyForCleanup;
  CFRunLoopPerformBlock(mRunLoop, kCFRunLoopDefaultMode, ^ {
    runLoopHasStarted->signal();
  });

  CFRunLoopWakeUp(mRunLoop);

  mEventStream = FSEventStreamCreate(
    NULL,
    &FSEventsServiceCallback,
    &callbackInfo,
    pathsToWatch,
    kFSEventStreamEventIdSinceNow,
    latency,
    // The extended data tells the inode of each event, which ties the two sides of a rename together.
    kFSEventStreamCreateFlagFileEvents | kFSEventStreamCreateFlagWatchRoot | kFSEventStreamCreateFlagUseCFTypes |
      kFSEventStreamCreateFlagUseExtendedData
  );

  FSEventStreamScheduleWithRunLoop(mEventStream, mRunLoop, kCFRunLoopDefaultMode);
  FSEventStreamStart(mEventStream);
  mStreamStarted.signal();
  CFRelease(pathsToWatch);
  for (CFStringRef watchedPath : watchedPaths) {
    if (watchedPath != NULL) CFRelease(watchedPath);
  }

  CFRunLoopRun();

  FSEventStreamStop(mEventStream);
  FSEventStreamUnscheduleFromRunLoop(mEventStream, mRunLoop, kCFRunLoopDefaultMode);
  FSEventStreamInvalidate(mEventStream);
  FSEventStreamRelease(mEventStream);

  mExited = true;
  mEventsService->runLoopExited();
}
