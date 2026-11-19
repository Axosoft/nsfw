#ifndef INOTIFY_TREE_H
#define INOTIFY_TREE_H
#include <sys/inotify.h>
#include <sys/stat.h>
#include <dirent.h>
#include <stdlib.h>
#include <fcntl.h>
#include <errno.h>
#include <sstream>
#include <vector>
#include <deque>
#include <future>
#include <map>
#include <set>
#include <cstdint>
#include <unordered_set>
#include <functional>
#include <atomic>
#include <chrono>
#include <thread>
#include <mutex>

/**
 * void EmitCreatedEvent(std::string directory, std::string file);
 */
using EmitCreatedEvent = std::function<void(std::string, std::string)>;

class InotifyTree {
public:
  InotifyTree(int inotifyInstance, std::string path, const std::vector<std::string> &excludedPaths);

  void addDirectory(int wd, std::string name, EmitCreatedEvent emitCreatedEvent = nullptr);
  std::string getError();
  bool getPath(std::string &out, int wd);
  bool hasErrored();
  bool isRootAlive();
  bool nodeExists(int wd);
  bool isRootExcluded() const;
  void removeDirectory(int wd, EmitCreatedEvent emitCreatedEvent);
  void forgetIncompleteScans();
  bool removeDroppedWatch(int wd);
  void removeDroppedWatches(int eventsRead);
  void retryWatches(EmitCreatedEvent emitCreatedEvent);
  void removeMovedDirectory(int wd, std::string name, EmitCreatedEvent emitCreatedEvent);
  void renameDirectory(int fromWd, std::string fromName, int toWd, std::string toName,
    EmitCreatedEvent emitCreatedEvent);
  void setError(const std::string &error);
  void watchIgnored(int wd);
  bool isQuarantined(int wd, uint32_t mask);
  void finishRead(int eventsRead, size_t bytesRead);
  void resumeFound();
  void cancelFound();
  bool needsEntryRetry() const { return !mChangedEntries.empty(); }
  void retryChangedEntry();
  std::future<bool> updateExcludedPaths(const std::vector<std::string> &excludedPaths, EmitCreatedEvent emitCreatedEvent);

  // In bytes: what one read of the event loop takes at most, which removeDroppedWatches() paces itself by.
  static const int READ_SIZE = 8192;

  ~InotifyTree();
private:
  class InotifyNode {
  public:
    InotifyNode(
      InotifyTree *tree,
      int inotifyInstance,
      InotifyNode *parent,
      const std::string &directory,
      std::string name,
      dev_t device,
      ino_t inodeNumber,
      bool throughSymlink,
      EmitCreatedEvent emitCreatedEvent = nullptr
    );

    void collectSightedBelow(std::vector<int> &holders);
    bool createChild(const std::string &name, EmitCreatedEvent emitCreatedEvent = nullptr, bool reportEntry = false);
    InotifyNode *findDescendant(const std::string &relativePath);
    void fixPaths();
    dev_t getDevice();
    std::string getFullPath();
    std::string getName();
    InotifyNode *getParent();
    int getWatchDescriptor();
    int addWatch(int attributes);
    void handleWatchFailure(int error);
    ino_t getInodeNumber();
    bool inotifyInit();
    bool registrationMatchesPath();
    bool isAlive();
    bool leadsToItself();
    InotifyNode *pullChild(std::string name);
    void removeChild(std::string name);
    void setName(std::string name);
    void setParent(InotifyNode *newParent);
    void takeChildAsName(InotifyNode *child, std::string name);
    bool scan(EmitCreatedEvent emitCreatedEvent, bool rescan = false, bool reportEntries = false);
    bool takeFoundAgain(bool &fromExclusionUpdate);
    bool foundByExclusionUpdate() const;
    bool registrationChanged() const { return mRegistrationChanged; }
    bool awaitsScan() const { return mFoundAgain; }
    uint64_t adoption = 0;
    bool rewatch(uint64_t marker, int &replaced);

    static std::string createFullPath(std::string parentPath, std::string name);

    ~InotifyNode();
  private:
    // IN_MASK_ADD: a path that leads to a directory already watched (the watched path, through a symlink, say) adds to
    // that watch's mask instead of replacing it, which would drop the IN_MOVE_SELF of the watched path's own.
    static const int ATTRIBUTES = IN_MASK_ADD
                                | IN_ATTRIB
                                | IN_CREATE
                                | IN_DELETE
                                | IN_MODIFY
                                | IN_MOVED_FROM
                                | IN_MOVED_TO
                                | IN_DELETE_SELF
                                | IN_ONLYDIR;

    bool mAlive;
    bool mRegistrationChanged = false;
    // Took a dropped watch back, and isn't scanned until the marker of its pass is queued, see settleFound().
    bool mFoundAgain = false;
    bool mFoundByExclusionUpdate = false;
    // Created at a symlink to a directory (or the watched path), which only changes when the symlink does.
    bool mThroughSymlink;
    std::map<std::string, InotifyNode *> *mChildren;
    const std::string &mDirectory;
    std::string mFullPath;
    dev_t mDevice;
    ino_t mInodeNumber;
    const int mInotifyInstance;
    std::string mName;
    InotifyNode *mParent;
    InotifyTree *mTree;
    int mWatchDescriptor;
    bool mWatchDescriptorInitialized;
  };

  void addNodeReferenceByWD(int watchDescriptor, InotifyNode *node);
  void removeNodeReferenceByWD(int watchDescriptor, InotifyNode *node);
  bool addInode(ino_t inodeNumber, InotifyNode *node);
  void removeInode(ino_t inodeNumber);
  InotifyNode * findNodeByInode(ino_t inodeNumber);
  InotifyNode * findNodeByPath(const std::string path);
  InotifyNode * findNodeAtTreePath(const std::string &path);
  InotifyNode * findAttachedAlias(const std::string &path);
  void collectPendingAncestors(std::string path, std::unordered_set<uint64_t> &ancestors);
  std::string getParentPath(const std::string &filePath);
  struct Sighting {
    int parentWatchDescriptor;
    std::string name;
    dev_t device;
    ino_t inodeNumber;
    bool throughSymlink;
    bool fromExclusionUpdate;
  };
  InotifyNode *findNodeByWatchDescriptor(int watchDescriptor);
  static bool isAncestorOrSelf(InotifyNode *ancestor, InotifyNode *node);
  void sight(InotifyNode *scanning, const std::string &name, const struct stat &file, bool throughSymlink);
  // What discard() watches once the leaving node is deleted: the sightings of it and of what it holds.
  struct DiscardSteps {
    std::vector<Sighting> reborn;
  };
  void rebornSighted(int watchDescriptor, InotifyNode *leaving, DiscardSteps &steps);
  void watchSighted(const Sighting &sighting, EmitCreatedEvent emitCreatedEvent);
  bool removeWatch(int wd, uint64_t marker);
  bool freeDroppedWatch();
  int roomInQueue(int queuedBytes);
  void discard(InotifyNode *node, EmitCreatedEvent emitCreatedEvent);
  int getQueuedBytes();
  void markScanIncomplete(InotifyNode *node, bool unreported = false);
  void rescanIncomplete(InotifyNode *moved, EmitCreatedEvent emitCreatedEvent);
  bool isExcluded(const std::string &path) const;
  bool isExcludedThroughAlias(InotifyNode *parent, const std::string &name);
  std::vector<int> applyExclusionsAfterMove(InotifyNode *node, const std::string &oldPath);
  void includeAgain(InotifyNode *parent, const std::string &name);
  bool existWatchedPath();
  bool isWatchedPathGone();

  // mError: the event loop thread records errors while it handles events, and so do the threads that add watches outside it
  // (the initial scan on start, updateExcludedPaths()). The poll thread reads it in getError(), and any thread calling
  // hasErrored() (the poll thread, inotifyInit() on the others) records the root path error there. Nothing else is
  // locked while holding this mutex, so it can't take part in a lock order problem with the event loop mutex.
  std::string mError;
  std::mutex mErrorMutex;
  const int mInotifyInstance;
  std::map<int, InotifyNode *> *mInotifyNodeByWatchDescriptor;
  std::map<ino_t, InotifyNode *> inodes;
  // The watches of directories dropped from the tree, not removed yet. None is in mInotifyNodeByWatchDescriptor:
  // their events may be from where the directory couldn't be watched. removeDroppedWatches() removes them a few at a
  // time, removeDroppedWatch() an excluded one at its next event, and freeDroppedWatch() some when the limit of watches
  // is reached. One that moved away stays until then: moved back, it takes its watch back behind the marker of its
  // pass, with the events from before that marker dropped, see InotifyNode::inotifyInit() and settleFound().
  struct DroppedWatch {
    bool excluded;
  };
  std::map<int, DroppedWatch> mDroppedWatches;
  static const int DROPPED_WATCHES_PER_BATCH = 64;
  bool mExcluding = false;
  bool mRestoringExclusions = false;
  struct RestorationScope {
    InotifyTree *tree;
    bool previous;
    RestorationScope(InotifyTree *tree, bool active): tree(tree), previous(tree->mRestoringExclusions) {
      tree->mRestoringExclusions = active;
    }
    ~RestorationScope() { tree->mRestoringExclusions = previous; }
  };
  // Removed watches awaiting IN_IGNORED and their marker sequence, if any.
  struct RemovedWatch {
    uint64_t removal;
    uint64_t marker;
  };
  std::map<int, RemovedWatch> mRemovedWatches;
  // From /proc, read once: what the queue holds, see roomInQueue().
  int mMaxQueuedEvents;
  // Kept free in the queue for the events of other processes, when freeing watches.
  static constexpr int QUEUE_MARGIN = 1024;
  // Directories that couldn't be watched for lack of room (see InotifyNode::inotifyInit()), and ones a scan saw whose
  // node went when its watch made a marker, once that marker is read (see mSightedBehindMarker): watched after a read
  // that leaves no move pending, see retryWatches().
  std::vector<Sighting> mRetries;
  // Where a scan saw a directory whose node went when its watch made the marker with this sequence, see queueMarker():
  // retried once that marker is read, after the events that tell where the directory went, if the kernel queued them
  // before it.
  std::vector<std::pair<uint64_t, Sighting>> mSightedBehindMarker;
  void releaseSighted(uint64_t marker);
  uint64_t mMarkerSequence = 0;
  // Watches taken back behind a marker, whose events are dropped until the marker with this sequence is read.
  std::map<int, uint64_t> mQuarantined;
  // The quarantine of a watch found again until its pass gets a marker.
  static constexpr uint64_t PASS_PENDING = UINT64_MAX;
  // Descendant scans inherit the update that discovered their parent. Its last pending scan releases the worker.
  struct UpdateCompletion {
    std::promise<bool> done;
    bool applied = true;
    ~UpdateCompletion() { done.set_value(applied); }
  };
  std::shared_ptr<UpdateCompletion> mCurrentUpdate;
  struct ChangedEntry {
    InotifyNode *parent;
    std::string name;
    EmitCreatedEvent emitCreatedEvent;
    bool restoring;
    std::shared_ptr<UpdateCompletion> update;
    uint64_t afterRemoval;
    uint64_t afterBytes;
  };
  std::deque<ChangedEntry> mChangedEntries;
  uint64_t mBytesHandled = 0;
  uint64_t mLastRemoval = 0;
  uint64_t mReadRemoval = 0;
  void deferChangedEntry(ChangedEntry entry);
  uint64_t mLastAdoption = 0;
  int mMarkerBatch = DROPPED_WATCHES_PER_BATCH;
  int mMarkerCredit = DROPPED_WATCHES_PER_BATCH;
  bool mDeferred = false;
  struct Found {
    uint64_t adoption;
    std::shared_ptr<UpdateCompletion> update;
    int watchDescriptor;
    EmitCreatedEvent emitCreatedEvent;
  };
  // The nodes found again in the pass, not scanned yet, see settleFound().
  std::vector<Found> mFound;
  int mCreating = 0;
  bool mSettling = false;
  uint64_t queueMarker(std::vector<Found> &pass);
  void settleFound();
  // Settles what an outermost creation found again, when it returns.
  struct Creating {
    InotifyTree *tree;
    explicit Creating(InotifyTree *tree): tree(tree) { ++tree->mCreating; }
    ~Creating() {
      if (--tree->mCreating == 0) {
        tree->settleFound();
      }
    }
  };
  // Read by the poll thread through isRootAlive() while the event loop thread clears it in removeDirectory().
  std::atomic<InotifyNode *> mRoot;
  // The nodes that missed a directory a rename still queued may have moved: one that went away while they scanned or
  // added it, or one they couldn't add because their path leads elsewhere, see rescanIncomplete(). Forgotten once no
  // event is queued, when every such rename has been handled. One whose node's path leads elsewhere then is kept for
  // STALE_SCAN_MARK_LIFETIME after the first such time (since), see forgetIncompleteScans(). unreported: a directory
  // found again whose scan, which would have reported what it holds, didn't run, so its rescan does, see settleFound().
  struct IncompleteScan {
    std::chrono::steady_clock::time_point since;
    bool unreported;
    bool fromExclusionUpdate;
  };
  std::map<InotifyNode *, IncompleteScan> mIncompleteScans;
  static constexpr std::chrono::seconds STALE_SCAN_MARK_LIFETIME = std::chrono::seconds(1);
  // Where a scan saw the directory of a node that is elsewhere in the tree, by the node's watch descriptor, see
  // sight().
  std::map<int, Sighting> mSightings;
  std::vector<std::string> mExcludedPaths;
  bool mRootExcluded;
  std::string mWatchedPath;

  friend class InotifyNode;
};

#endif
