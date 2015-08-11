#include "../includes/FileWatcherLinux.h"

namespace NSFW {

  FileWatcherLinux::FileWatcherLinux(std::string path, std::queue<Event> &eventsQueue, bool &watchFiles)
    : mEventsQueue(eventsQueue), mPath(path), mWatchFiles(watchFiles) {}
  FileWatcherLinux::~FileWatcherLinux() {}
  bool FileWatcherLinux::start() {
    return false;
  }
  void FileWatcherLinux::stop() {}

}
