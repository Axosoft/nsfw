#ifndef NSFW_FS_EVENTS_SERVICE_H
#define NSFW_FS_EVENTS_SERVICE_H

#include "RunLoop.h"
#include "../Queue.h"

#include <CoreServices/CoreServices.h>
#include <time.h>
#include <sys/stat.h>
#include <list>
#include <map>
#include <mutex>
#include <unordered_map>
#include <vector>
#include <string>

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
  FSEventsService(std::shared_ptr<EventQueue> queue, std::string path, const std::vector<std::string> &excludedPaths,
    bool recursive = true);

  friend void FSEventsServiceCallback(
    ConstFSEventStreamRef streamRef,
    void *clientCallBackInfo,
    size_t numEvents,
    void *eventPaths,
    const FSEventStreamEventFlags eventFlags[],
    const FSEventStreamEventId eventIds[]
  );
  std::string getError();
  bool hasErrored();
  bool isWatching();
  // The run loop thread ended: on its own it can't report anything anymore, which hasErrored() tells.
  void runLoopExited();
  void updateExcludedPaths(const std::vector<std::string> &excludedPaths);

  ~FSEventsService();
private:
  // One FSEvents event. inode: the file system object it is about, 0 when FSEvents didn't tell.
  struct Event {
    std::string path;
    FSEventStreamEventFlags flags;
    FSEventStreamEventId id;
    uint64_t inode;
  };

  // What the path of an event with the renamed flag holds now, and the other side of its rename once paired.
  struct RenameSide {
    bool renamed = false;
    bool exists = false;
    // It holds the entry the event is about: FSEvents didn't tell which, or it is the same.
    bool present = false;
    uint64_t inode = 0;
    PathFilter::Kind kind = PathFilter::Kind::UNKNOWN;
    // The volume of the entry, or of the nearest directory still there above the place it left.
    dev_t device = 0;
    long partner = -1;
    bool hardLinked = false;
    bool from = false;
    // A path the entry only passed through, in a chain of renames within the batch.
    bool transient = false;
    // The entry left the path and came back within the batch.
    bool returned = false;
    // Another event in the batch at the same path, about the same entry if renamed, speaks for this one.
    bool repeated = false;
    // What the path holds now is not the entry, and another side in the batch reports it.
    bool replaced = false;
  };

  static bool readEventData(const void *data, std::string &path, uint64_t &inode);
  void handle(std::vector<Event> events);
  static void pairSides(std::vector<RenameSide> &sides, size_t from, size_t to);
  void pairRenames(const std::vector<Event> &events, std::vector<RenameSide> &sides);
  void pairCaseRenames(const std::vector<Event> &events, const std::vector<size_t> &present, size_t source,
    std::vector<RenameSide> &sides);
  std::string storedName(const std::string &path);
  void passThrough(const Event &event, const RenameSide &side);
  bool isSinceStart(const struct timespec &time) const;
  bool readAddedTime(const std::string &path, uint64_t inode, struct timespec &added) const;
  bool appearedWhileWatching(const Event &event, const struct stat &file) const;
  void classify(const Event &event, PathFilter::Kind kind);
  void noteExcluded(const Event &event);
  void create(const std::string &path, uint64_t inode, PathFilter::Kind kind = PathFilter::Kind::UNKNOWN);
  void dispatch(EventType action, std::string path, PathFilter::Kind kind = PathFilter::Kind::UNKNOWN);
  void modify(std::string path);
  // inode: of the entry gone, 0 if unknown.
  void remove(std::string path, PathFilter::Kind kind = PathFilter::Kind::UNKNOWN, uint64_t inode = 0);
  void noteGone(const std::string &path, uint64_t inode);
  bool wasReportedGone(const Event &event) const;
  void rememberMissing(const std::string &path, uint64_t inode, bool pendingCreation);
  void forgetGone(const std::string &path);
  void rename(const std::string &from, const std::string &to, uint64_t inode, PathFilter::Kind kind);
  bool lookUpReported(const std::string &path, uint64_t &inode);
  void noteReported(const std::string &path, uint64_t inode);
  void splitFilePath(std::string &directory, std::string &name, const std::string &path);
  bool isExcluded(std::string filePath);
  bool isOutsideChildren(const char *filePath) const;
  CFStringCompareFlags pathCompareFlags() const;
  std::vector<CFStringRef> createExcludedPaths(const std::vector<std::string> &excludedPaths) const;
  bool excludesRoot(const std::vector<CFStringRef> &excludedPaths) const;
  static void releaseExcludedPaths(const std::vector<CFStringRef> &excludedPaths);

  std::string mPath;
  RunLoop *mRunLoop;
  std::shared_ptr<EventQueue> mQueue;
  // Read on the FSEvents run loop thread, replaced from a worker thread by updateExcludedPaths.
  std::mutex mExcludedPathsLock;
  std::vector<CFStringRef> mExcludedPaths;
  bool mRootExcluded;
  bool mRootChanged;
  bool mCaseSensitive;
  bool mRecursive;
  // When the stream started: an entry born before then was there before watching.
  struct timespec mStartTime;

  // The inode of the entry the watcher reported at each path where it reported one that it didn't report gone since,
  // with the directory it is in, or saw one appear while excluded. FSEvents repeats the flags of the earlier changes
  // of a path with each new one, so this tells a change from the creation or the removal reported before it. It grows
  // with the entries that appeared while watching and are still there, not with the watched tree. Ordered, so what a
  // directory holds is a range.
  std::map<std::string, uint64_t> mReported;
  // The inode of what the watcher reported gone at each of the latest paths it did, 0 if it didn't know: FSEvents
  // repeats the flags of that removal with the next events of the path, which then aren't news. Bounded, as
  // forgetting one only risks reporting that removal again; the oldest first.
  struct GoneEntry {
    uint64_t inode;
    bool pendingCreation;
    std::list<std::string>::iterator position;
  };
  std::unordered_map<std::string, GoneEntry> mGone;
  std::list<std::string> mGoneOrder;
  // The latest event id when the stream was created: an event up to it is about a change made before watching.
  FSEventStreamEventId mStartEventId;
};

#endif
