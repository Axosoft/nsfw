#ifndef NSFW_QUEUE_H
#define NSFW_QUEUE_H

#include <string>
#include <memory>
#include <deque>
#include <vector>
#include <mutex>
#include <atomic>

#include "PathFilter.h"

enum EventType {
  CREATED = 0,
  DELETED = 1,
  MODIFIED = 2,
  RENAMED = 3
};

struct Event {
  Event(
    const EventType type,
    const std::string &fromDirectory,
    const std::string &fromFile,
    const std::string &toDirectory,
    const std::string &toFile
  ) :
    type(type),
    fromDirectory(fromDirectory),
    toDirectory(toDirectory),
    fromFile(fromFile),
    toFile(toFile)
  {}

  EventType type;
  std::string fromDirectory, toDirectory, fromFile, toFile;
};

class EventQueue {
public:
  void clear();
  std::size_t count();
  std::unique_ptr<Event> dequeue();
  std::unique_ptr<std::vector<std::unique_ptr<Event>>> dequeueAll();
  // kind: what the entry is, when the watcher knows: a removed one can't be looked up anymore.
  void enqueue(
    const EventType type,
    const std::string &fromDirectory,
    const std::string &fromFile,
    const std::string &toDirectory = "",
    const std::string &toFile = "",
    PathFilter::Kind kind = PathFilter::Kind::UNKNOWN
  );

  void pause();
  void resume();
  // Drops the events of the paths it ignores, and turns a rename across its boundary into a creation or a removal.
  // Set by the run that watches, before its watcher starts, and reset after it stops. Kept while inactive: an update
  // of its patterns can make it active. watcherNotesEntries: the watcher tells the filter about every entry change
  // itself, before it acts on it; otherwise the queue does, for the events it is given.
  void setFilter(std::shared_ptr<PathFilter> filter, bool watcherNotesEntries = false);

  EventQueue();

private:
  void push(
    const EventType type,
    const std::string &fromDirectory,
    const std::string &fromFile,
    const std::string &toDirectory = "",
    const std::string &toFile = ""
  );
  std::shared_ptr<PathFilter> mFilter;
  bool mWatcherNotesEntries = false;
  std::mutex mFilterMutex;
  std::deque<std::unique_ptr<Event>> queue;
  std::mutex mutex;
  std::atomic<bool> mPaused;
};

#endif
