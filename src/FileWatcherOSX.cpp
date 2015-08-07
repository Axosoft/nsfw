#include "../includes/FileWatcherOSX.h"
#include <iostream>
namespace NSFW {

  FileWatcherOSX::FileWatcherOSX(std::string path, std::queue<Event> &eventsQueue, bool &watchFiles)
    : mEventsQueue(eventsQueue), mPath(path), mWatchFiles(watchFiles) {}

  std::string FileWatcherOSX::getPath() {
    return mPath;
  }

  bool FileWatcherOSX::start() {
    if (pthread_create(&mThread, 0, &FileWatcherOSX::mainLoop, (void *)this)) {
      return true;
    } else {
      return false;
    }
  }

  void FileWatcherOSX::callback(
      ConstFSEventStreamRef streamRef,
      void *clientCallBackInfo,
      size_t numEvents,
      void *eventPaths,
      const FSEventStreamEventFlags eventFlags[],
      const FSEventStreamEventId eventIds[])
  {
      int i;
      char **paths = (char **)eventPaths;

      FileWatcherOSX *fwOSX = (FileWatcherOSX *)clientCallBackInfo;

      for (i=0; i<numEvents; i++) {
          int count;
          printf("Change %llu in %s, flags %lu\n", eventIds[i], paths[i], eventFlags[i]);
     }
  }

  void *FileWatcherOSX::mainLoop(void *params) {
    FileWatcherOSX *fwOSX = (FileWatcherOSX *)params;
    CFStringRef mypath = CFStringCreateWithCString(
      NULL,
      (char *)(fwOSX->getPath().c_str()),
      kCFStringEncodingUTF8);
    CFArrayRef pathsToWatch = CFArrayCreate(NULL, (const void **)&mypath, 1, NULL);
    FSEventStreamRef stream;
    CFAbsoluteTime latency = 1.0;

    FSEventStreamContext callbackInfo;
    callbackInfo.version = 0;
    callbackInfo.info = params;

    /* Create the stream, passing in a callback */
    stream = FSEventStreamCreate(NULL,
        &FileWatcherOSX::callback,
        &callbackInfo,
        pathsToWatch,
        kFSEventStreamEventIdSinceNow, /* Or a previous event ID */
        latency,
        kFSEventStreamCreateFlagNone /* Flags explained in reference */
    );



    FSEventStreamScheduleWithRunLoop(stream, CFRunLoopGetCurrent(), kCFRunLoopDefaultMode);
    FSEventStreamStart(stream);
    CFRunLoopRun();
  }

}
