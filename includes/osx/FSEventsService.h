#ifndef NSFW_FS_EVENTS_SERVICE_H
#define NSFW_FS_EVENTS_SERVICE_H

#include "RunLoop.h"

#include <CoreServices/CoreServices.h>
#include <time.h>
#include <sys/stat.h>
#include <map>
#include <vector>
#include <string>
#include <iostream>

void FSEventsServiceCallback(
  ConstFSEventStreamRef streamRef,
  void *clientCallBackInfo,
  size_t numEvents,
  void *eventPaths,
  const FSEventStreamEventFlags eventFlags[],
  const FSEventStreamEventId eventIds[]
);

class RunLoop;

class FSEventsService {
public:
  FSEventsService(std::string path);

  friend void FSEventsServiceCallback(
    ConstFSEventStreamRef streamRef,
    void *clientCallBackInfo,
    size_t numEvents,
    void *eventPaths,
    const FSEventStreamEventFlags eventFlags[],
    const FSEventStreamEventId eventIds[]
  );

  ~FSEventsService();
private:
  void create(std::string path);
  void demangle(std::string path);
  void modify(std::string path);
  void remove(std::string path);
  void rename(std::vector<std::string> *paths);
  void splitFilePath(std::string &directory, std::string &name, std::string path);

  RunLoop *mRunLoop;
  bool mWatching;
};

#endif
