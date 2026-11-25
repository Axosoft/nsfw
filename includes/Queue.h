#ifndef NSFW_QUEUE_H
#define NSFW_QUEUE_H

#include <string>
#include <memory>
#include <deque>
#include <vector>
#include <mutex>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdint>

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
  std::unique_ptr<std::vector<std::unique_ptr<Event>>> dequeueAll(uint64_t &epoch);
  uint64_t epoch() const { return mEpoch; }
  // kind: what the entry is, when the watcher knows: a removed one can't be looked up anymore.
  void enqueue(
    const EventType type,
    const std::string &fromDirectory,
    const std::string &fromFile,
    const std::string &toDirectory = "",
    const std::string &toFile = "",
    PathFilter::Kind kind = PathFilter::Kind::UNKNOWN
  );

  // Keep a native read together so a removal and its replacement can be delivered in one batch.
  void holdEvents();
  // Returns whether a removal was kept for the next read to pair with its replacement.
  bool releaseEvents(bool keepTrailingRemoval = false);

  void pause();
  void resume();

  // What wakes the delivery thread of the run that watches, which sleeps until one of them comes instead of checking
  // on a timer. WAKE_EVENTS comes with every event queued, or once a hold of them is released, WAKE_ERROR from the
  // watcher when it records an error, and WAKE_STOP from the run when it ends.
  enum Wake : unsigned { WAKE_EVENTS = 1, WAKE_ERROR = 2, WAKE_STOP = 4 };
  void wake(unsigned reasons);
  // Blocks until a wake for one of reasons comes or the deadline passes, and takes the ones that came, which the
  // later calls don't see. The wakes for other reasons stay for them, and don't interrupt this one.
  unsigned waitForWake(unsigned reasons, std::chrono::steady_clock::time_point deadline =
    std::chrono::steady_clock::time_point::max());
  // Forgets the wakes a previous run left, so they don't reach the next one.
  void resetWakes();
  // Drops the events of the paths it ignores, and turns a rename across its boundary into a creation or a removal.
  // Set by the run that watches, before its watcher starts, and reset after it stops. Kept while inactive: an update
  // of its patterns can make it active. watcherNotesEntries: the watcher tells the filter about every entry change
  // itself, before it acts on it; otherwise the queue does, for the events it is given.
  void setFilter(std::shared_ptr<PathFilter> filter, bool watcherNotesEntries = false);

  EventQueue();

private:
  void push(
    uint64_t epoch,
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
  // Under mutex: whether events are held, and how many at the front of the queue can be dequeued meanwhile.
  bool mHolding = false;
  std::size_t mHeldFrom = 0;
  std::mutex mutex;
  // A pause or stop invalidates batches that have already left the queue for JavaScript.
  std::atomic<uint64_t> mEpoch{0};
  std::atomic<bool> mPaused;
  std::mutex mWakeMutex;
  std::condition_variable mWakeCondition;
  unsigned mWakes = 0;
  unsigned mWaitingFor = 0;
};

#endif
