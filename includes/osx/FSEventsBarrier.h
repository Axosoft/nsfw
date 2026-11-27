#ifndef NSFW_FS_EVENTS_BARRIER_H
#define NSFW_FS_EVENTS_BARRIER_H

#include <string>

// A private directory under the user's temporary directory, outside any watched tree, for a watcher's stream to watch
// too: a file created there is reported after every change made before, which FSEventStreamFlushSync() alone doesn't
// ensure, see FSEventsService::flush(). Its real path, as FSEvents reports it; empty if it couldn't be made.
std::string createFSEventsBarrierDirectory();

// Whether path is in one of those directories, which a watcher of the temporary directory would see.
bool isFSEventsBarrierPath(const std::string &path);

#endif
