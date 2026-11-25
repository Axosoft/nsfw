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
#include "SymlinkIndex.h"

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
    const std::string &toFile,
    const PathFilter::Kind kind
  ) :
    type(type),
    fromDirectory(fromDirectory),
    toDirectory(toDirectory),
    fromFile(fromFile),
    toFile(toFile),
    kind(kind)
  {}

  EventType type;
  std::string fromDirectory, toDirectory, fromFile, toFile;
  // What the entry itself is, not what a symlink leads to: what tells removing it with unlink from removing it as a
  // directory. A symlink is a FILE, except on Windows, where a directory symlink or junction is a DIRECTORY.
  PathFilter::Kind kind;
};

class EventQueue {
public:
  void clear();
  std::size_t count();
  std::unique_ptr<Event> dequeue();
  std::unique_ptr<std::vector<std::unique_ptr<Event>>> dequeueAll(uint64_t &epoch);
  uint64_t epoch() const { return mEpoch; }
  // filterKind: what the filter judges the entry as when it is gone, following a symlink as the watchers do; a removed
  // one can't be looked up anymore. kind: what is reported, see Event::kind; for a rename, the entry that moved.
  void enqueue(
    const EventType type,
    const std::string &fromDirectory,
    const std::string &fromFile,
    const std::string &toDirectory = "",
    const std::string &toFile = "",
    PathFilter::Kind filterKind = PathFilter::Kind::UNKNOWN,
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
  // Follows the symlinks that lead to files in the watched tree, and reports a change of such a file under them too,
  // right after the change itself. Set and reset with the filter.
  void setSymlinks(std::shared_ptr<SymlinkIndex> symlinks);

  EventQueue();

private:
  void judge(
    uint64_t eventEpoch,
    const EventType type,
    const std::string &fromDirectory,
    const std::string &fromFile,
    const std::string &toDirectory,
    const std::string &toFile,
    PathFilter::Kind filterKind,
    PathFilter::Kind kind
  );
  void judgeDerived(uint64_t eventEpoch, const SymlinkIndex::Derived &derived);
  void push(
    uint64_t epoch,
    const EventType type,
    const std::string &fromDirectory,
    const std::string &fromFile,
    const std::string &toDirectory,
    const std::string &toFile,
    PathFilter::Kind kind
  );
  std::shared_ptr<PathFilter> mFilter;
  std::shared_ptr<SymlinkIndex> mSymlinks;
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
