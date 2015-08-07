#include "../includes/FileWatcherOSX.h"

namespace NSFW {

FileWatcherOSX::FileWatcherOSX(std::string path, std::queue<Event> &eventsQueue, bool &watchFiles)
  : mEventsQueue(eventsQueue), mPath(path), mWatchFiles(watchFiles) {}

}
