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
#include <unordered_map>
#include <unordered_set>
#include <functional>
#include <atomic>
#include <chrono>
#include <thread>
#include <mutex>

#include "../PathFilter.h"

/**
 * void EmitCreatedEvent(std::string directory, std::string file, PathFilter::Kind kind);
 */
using EmitCreatedEvent = std::function<void(std::string, std::string, PathFilter::Kind)>;
// A symlink found while listing a directory, that isn't excluded.
using LinkFound = std::function<void(const std::string &directory, const std::string &name)>;
/**
 * void ReportMove(bool renamed); false: the moved directory left for a place that isn't watched.
 */
using ReportMove = std::function<void(bool)>;

class InotifyTree {
  // The last names of excluded paths.
  using ExcludedNames = std::unordered_set<std::string>;
  // A directory, wherever a path to it is.
  using Directory = std::pair<dev_t, ino_t>;
  struct DirectoryHash {
    size_t operator()(const Directory &directory) const {
      return std::hash<ino_t>()(directory.second) ^ (std::hash<dev_t>()(directory.first) * 0x9e3779b97f4a7c15ULL);
    }
  };
public:
  // errorRecorded: called once, by the thread that records the first error, outside the tree's locks.
  InotifyTree(int inotifyInstance, std::string path, const std::vector<std::string> &excludedPaths,
    std::shared_ptr<PathFilter> filter, std::function<void()> errorRecorded, LinkFound linkFound = nullptr);

  void addDirectory(int wd, std::string name, EmitCreatedEvent emitCreatedEvent = nullptr);
  std::string getError();
  bool getPath(std::string &out, int wd);
  bool hasErrored();
  bool isRootAlive();
  bool nodeExists(int wd);
  bool isRootExcluded() const;
  void removeDirectory(int wd, EmitCreatedEvent emitCreatedEvent);
  void forgetIncompleteScans();
  bool isReportedByScan(int wd, const std::string &name) const;
  void startRead(size_t bytesRead);
  bool removeDroppedWatch(int wd);
  void removeDroppedWatches(int eventsRead);
  void retryWatches(EmitCreatedEvent emitCreatedEvent);
  void removeMovedDirectory(int wd, std::string name, EmitCreatedEvent emitCreatedEvent);
  bool renameDirectory(int fromWd, std::string fromName, int toWd, std::string toName,
    EmitCreatedEvent emitCreatedEvent, const ReportMove &reportMove);
  void setError(const std::string &error);
  void watchIgnored(int wd);
  bool isQuarantined(int wd, uint32_t mask);
  void finishRead(int eventsRead, size_t bytesRead);
  void resumeFound();
  void cancelFound();
  bool needsEntryRetry() const { return !mChangedEntries.empty(); }
  void entryChanged(int wd, const std::string &name, bool removed, EmitCreatedEvent emitCreatedEvent);
  void entryDropped(int wd, const std::string &name);
  void revalidateRejected(EmitCreatedEvent emitCreatedEvent);
  void revalidateLinks(bool unknownMoveDestination, EmitCreatedEvent emitCreatedEvent);
  void retryChangedEntry();
  std::future<bool> updateExcludedPaths(const std::vector<std::string> &excludedPaths, EmitCreatedEvent emitCreatedEvent);
  void excludeFiltered(const std::vector<std::string> &directories, EmitCreatedEvent emitCreatedEvent,
    bool holderSubtrees = true);
  void includeFiltered();
  void recheckFilteredLinks(EmitCreatedEvent emitCreatedEvent);
  // The physical path of the directory with wd when a symlink holds it or one above it, see physicalPathOf(node).
  bool physicalPathOf(int wd, std::string &physical);
  // Whether name in the directory with wd led to a directory as far as the tree knows: it watches it, or rejected it.
  bool hadDirectoryAt(int wd, const std::string &name);
  // Whether the tree holds the directory with wd at a symlink that leads to it.
  bool isHeldAtLink(int wd);

  // In bytes: what one read of the event loop takes at most, which removeDroppedWatches() paces itself by.
  static const int READ_SIZE = 8192;

  ~InotifyTree();
private:
  class InotifyNode {
  public:
    // fullPath: this node's, stored only for a node with no parent: a child's is derived from its parent's.
    InotifyNode(
      InotifyTree *tree,
      InotifyNode *parent,
      const std::string &fullPath,
      std::string name,
      dev_t device,
      ino_t inodeNumber,
      bool throughSymlink,
      EmitCreatedEvent emitCreatedEvent = nullptr
    );

    // Calls visit with each node below this one, a parent before its children. It mustn't change the tree.
    template <typename Visit> void forEachBelow(const Visit &visit) {
      for (InotifyNode *child : mChildren) {
        visit(child);
        child->forEachBelow(visit);
      }
    }
    // fullPath: this node's, which the caller builds once for what it does with the node.
    bool createChild(const std::string &fullPath, const std::string &name, EmitCreatedEvent emitCreatedEvent = nullptr,
      bool reportEntry = false, uint64_t reportedUntil = 0, unsigned char type = DT_UNKNOWN);
    InotifyNode *findDescendant(const std::string &relativePath);
    dev_t getDevice();
    std::string getFullPath();
    const std::string &getName() const;
    InotifyNode *getParent();
    int getWatchDescriptor();
    int addWatch(int attributes, const std::string &fullPath);
    void handleWatchFailure(int error);
    ino_t getInodeNumber();
    bool inotifyInit(const std::string &fullPath);
    bool registrationMatchesPath(const std::string &fullPath);
    bool isAlive();
    bool leadsToItself();
    bool leadsToItself(const std::string &fullPath);
    // At a symlink that no longer leads to its directory.
    bool isAtStaleLink();
    InotifyNode *pullChild(std::string name);
    void removeChild(std::string name);
    void takeChildAsName(InotifyNode *child, std::string name);
    bool scan(const std::string &fullPath, EmitCreatedEvent emitCreatedEvent, bool rescan = false,
      bool reportEntries = false);
    bool takeFoundAgain(bool &fromExclusionUpdate);
    bool foundByExclusionUpdate() const;
    bool registrationChanged() const { return mRegistrationChanged; }
    bool awaitsScan() const { return mFoundAgain; }
    bool isThroughSymlink() const { return mThroughSymlink; }
    void setThroughSymlink(bool throughSymlink) { mThroughSymlink = throughSymlink; }
    bool rewatch(uint64_t marker, int &replaced);

    static std::string createFullPath(const std::string &parentPath, const std::string &name);

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

    // Orders the children by name, which can be looked up without a node: the name is only kept in the child.
    struct ByName {
      using is_transparent = void;
      bool operator()(const InotifyNode *a, const InotifyNode *b) const { return a->mName < b->mName; }
      bool operator()(const InotifyNode *a, const std::string &b) const { return a->mName < b; }
      bool operator()(const std::string &a, const InotifyNode *b) const { return a < b->mName; }
    };
    void adopt(InotifyNode *child);
    // Keeps the path of a node without a parent: leaving one, it keeps the path it had there until another takes it.
    void setParent(InotifyNode *newParent);

    // The path isn't kept: it is the parent's and the name, see getFullPath(), so a rename changes no path below it.
    std::set<InotifyNode *, ByName> mChildren;
    std::string mName;
    InotifyNode *mParent;
    InotifyTree *mTree;
    dev_t mDevice;
    ino_t mInodeNumber;
    int mWatchDescriptor;
    // Bitfields keep the node in a smaller malloc bin. C++17 has no default initializers for them: the ctor sets them.
    bool mWatchDescriptorInitialized : 1;
    bool mAlive : 1;
    bool mRegistrationChanged : 1;
    // Took a dropped watch back, and isn't scanned until the marker of its pass is queued, see settleFound().
    bool mFoundAgain : 1;
    bool mFoundByExclusionUpdate : 1;
    // At a symlink to a directory (or the watched path), which only changes when the symlink does.
    bool mThroughSymlink : 1;
  };

  void addNodeReferenceByWD(int watchDescriptor, InotifyNode *node);
  void removeNodeReferenceByWD(int watchDescriptor, InotifyNode *node);
  InotifyNode * findNodeOf(const struct stat &file);
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
  void sight(InotifyNode *scanning, const std::string &scanningPath, const std::string &name, const struct stat &file,
    bool throughSymlink);
  InotifyNode *findSightedAt(int wd, const std::string &name);
  InotifyNode *findMovedNode(int wd, const std::string &name);
  bool untangleMove(int fromWd, const std::string &fromName, int toWd, EmitCreatedEvent emitCreatedEvent);
  InotifyNode *findPlace(const Sighting &sighting);
  void setSighting(int holder, const Sighting &sighting);
  void placeSighting(int holder, const Sighting &sighting);
  void unlinkSighting(int holder);
  void moveSighting(int holder, int toWd, const std::string &toName);
  void moveSightingsIn(int fromWd, int toWd);
  void eraseSighting(int holder);
  using Lookup = std::pair<Directory, std::string>;
  bool knowsOwnPlace(int holder);
  void traceLinkWay(int holder);
  void untraceLinkWay(int holder);
  std::vector<Lookup> traceLinkLookups(const std::string &from, const Directory &directory, const std::string &path);
  void queueLinksLookingUp(InotifyNode *directory, const std::string &name);
  void settleChangedLinks(EmitCreatedEvent emitCreatedEvent);
  bool awaitsMoveAbove(InotifyNode *holder);
  struct SightedBelow {
    std::vector<int> held;
    std::vector<int> seenIn;
  };
  SightedBelow findSightedBelow(InotifyNode *node);
  // What is left to do once a node is deleted, for the directories that it or the nodes below it held and that stay:
  // the ones that took their own place have the excluded paths applied there, the others are watched where a scan saw
  // them.
  struct DiscardSteps {
    std::vector<int> rehomed;
    std::vector<Sighting> reborn;
    // Nodes held at symlinks elsewhere whose directories a scan saw in it, see settleStranded().
    std::vector<int> stranded;
  };
  enum class OwnPlace { NONE, EXCLUDED, TAKEN };
  OwnPlace takeOwnPlace(InotifyNode *holder, InotifyNode *leaving);
  OwnPlace moveToOwnPlace(InotifyNode *holder, EmitCreatedEvent emitCreatedEvent);
  void settleAtOwnPlace(InotifyNode *node, EmitCreatedEvent emitCreatedEvent);
  void settleLink(InotifyNode *holder, EmitCreatedEvent emitCreatedEvent);
  void settleLinkOf(int watchDescriptor, EmitCreatedEvent emitCreatedEvent);
  void settleLinksTo(InotifyNode *moved, EmitCreatedEvent emitCreatedEvent);
  // Why a node goes: its directory left the watched path (or was deleted), or a rename replaced it there.
  enum class Departure { LEFT, REPLACED };
  void keepSightedBelow(InotifyNode *node, Departure departure, DiscardSteps &steps);
  void settleKept(const DiscardSteps &steps, EmitCreatedEvent emitCreatedEvent);
  void settleStranded(const std::vector<int> &stranded, EmitCreatedEvent emitCreatedEvent);
  void rebornSighted(int watchDescriptor, InotifyNode *leaving, DiscardSteps &steps);
  void watchSighted(const Sighting &sighting, EmitCreatedEvent emitCreatedEvent);
  bool removeWatch(int wd, uint64_t marker);
  bool freeDroppedWatch();
  int roomInQueue(int queuedBytes);
  void discard(InotifyNode *node, EmitCreatedEvent emitCreatedEvent);
  void discardNode(InotifyNode *node, Departure departure, EmitCreatedEvent emitCreatedEvent);
  int getQueuedBytes();
  void markScanIncomplete(InotifyNode *node, bool unreported = false);
  void rescanIncomplete(InotifyNode *moved, EmitCreatedEvent emitCreatedEvent);
  bool isExcluded(const std::string &path) const;
  void setExcludedPaths(const std::vector<std::string> &excludedPaths);
  size_t findNamesBelowWatchedPath(const std::string &path) const;
  InotifyNode *findRoot(InotifyNode *node);
  struct Way;
  void followWay(Way &way, InotifyNode *root);
  void indexWay(Way &way, bool indexed);
  bool lookupChanged(InotifyNode *directory, const std::string &name);
  void refreshEntryLookups(InotifyNode *directory, const std::string &name);
  void excludedEntryFound(const std::string &excludedPath);
  void directoryChanged(const Directory &directory, InotifyNode *node);
  bool namesThroughAlias(InotifyNode *parent, const std::string &name);
  // BY_FILTER: the filter ignores it, by the tree's own path too, but unlike an excluded path it never makes a symlink
  // to it excluded, see excludesLink().
  enum class Rejected { NO, BY_PATH, BY_ALIAS, BY_FILTER };
  enum class Link { UNKNOWN, NO, YES };
  Rejected exclusionOfChild(InotifyNode *parent, const std::string &name, Link link = Link::UNKNOWN,
    bool askFilter = true, bool *throughLink = nullptr);
  bool isFilterRejectedOnly(InotifyNode *parent, const std::string &name) const;
  void forgetFilterRejection(InotifyNode *parent, const std::string &name);
  bool leadsBelowAny(InotifyNode *holder, const std::vector<std::string> &directories);
  bool physicalPathOf(InotifyNode *node, std::string &physical);
  Rejected rejectionOfChild(InotifyNode *parent, const std::string &name, Link link = Link::UNKNOWN);
  bool rejectsChild(InotifyNode *parent, const std::string &name, Link link = Link::UNKNOWN);
  bool isRejected(InotifyNode *parent, const std::string &name) const;
  void setRejected(InotifyNode *parent, const std::string &name, Rejected rejected);
  void applyExclusions(InotifyNode *node, EmitCreatedEvent emitCreatedEvent);
  // Which rejected names includeRejected() asks about again.
  enum class Rejections { ALL, BY_ALIAS, BY_FILTER };
  void includeRejected(InotifyNode *node, Rejections which, EmitCreatedEvent emitCreatedEvent = nullptr);
  void restoreRejectedChild(InotifyNode *parent, const std::string &name, EmitCreatedEvent emitCreatedEvent);
  bool excludeNamed(InotifyNode *node, const ExcludedNames &names, EmitCreatedEvent emitCreatedEvent,
    bool byFilter = true);
  bool excludesLink(const std::string &path, const Directory &directory, std::string *target = nullptr);
  void loadPhysicalExclusions();
  bool excludeLink(InotifyNode *node, EmitCreatedEvent emitCreatedEvent);
  bool excludeLinks(EmitCreatedEvent emitCreatedEvent, bool reconcileStale, const std::set<std::string> &changedRoots);
  void includeLinks(EmitCreatedEvent emitCreatedEvent);
  struct PhysicalExclusion {
    std::string path;
    std::string physical;
    Directory directory;
  };
  bool findStaleLinkTargets(const PhysicalExclusion &excluded, std::map<Directory, int> &stale,
    EmitCreatedEvent emitCreatedEvent);
  void findStaleLinksByTarget(std::map<Directory, int> &stale, EmitCreatedEvent emitCreatedEvent);
  bool discardStaleLinkAt(const PhysicalExclusion &excluded, const std::string &path, const Directory &identity,
    int watchDescriptor, EmitCreatedEvent emitCreatedEvent);
  bool leadsToWatchedDirectory();
  bool isWatchedPathGone();

  // mError: the event loop thread records errors while it handles events, and so do the threads that add watches outside it
  // (the initial scan on start, updateExcludedPaths()). The delivery thread reads it in getError(), and any thread
  // calling hasErrored() (the event loop thread before handling each read and while idle, the delivery thread,
  // inotifyInit() on the others) records the root path error there. Nothing else is locked while holding this mutex,
  // so it can't take part in a lock order problem with the event loop mutex.
  std::string mError;
  std::mutex mErrorMutex;
  std::function<void()> mErrorRecorded;
  const int mInotifyInstance;
  std::unordered_map<int, InotifyNode *> mInotifyNodeByWatchDescriptor;
  // A null value reserves a directory while its node is being constructed.
  std::unordered_map<Directory, InotifyNode *, DirectoryHash> mNodesByDirectory;
  // The path of each node with no parent: the root, and a node pulled from its parent until another one takes it,
  // whose path (and the paths below it) stays where it was, see InotifyNode::getFullPath().
  std::unordered_map<const InotifyNode *, std::string> mPathsOfTopNodes;
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
  // mBytesHandled once the read being handled is: what FIONREAD counts from, see scanReportsUntil().
  uint64_t mBytesTaken = 0;
  uint64_t mLastRemoval = 0;
  uint64_t mReadRemoval = 0;
  void deferChangedEntry(ChangedEntry entry);
  uint64_t mLastAdoption = 0;
  // The adoption of each node found again, until settleFound() takes it for its scan or the node goes: kept out of the
  // nodes, since few have one at a time. 0 for the rest.
  std::unordered_map<const InotifyNode *, uint64_t> mAdoptions;
  uint64_t adoptionOf(const InotifyNode *node) const;
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
        tree->mPhysicalExclusionsLoaded = false;
        tree->settleFound();
      }
    }
  };
  // Read by the delivery thread through isRootAlive() while the event loop thread clears it in removeDirectory().
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
  // The entries a scan reported as created in the directory with this watch descriptor, which already watched it: one
  // created after the watch was added and before the scan listed it has its IN_CREATE queued, see isReportedByScan().
  // untilBytes: the mBytesHandled once every event queued when they were listed is handled, see scanReportsUntil().
  struct ScanReports {
    std::unordered_set<std::string> names;
    uint64_t untilBytes = 0;
  };
  std::unordered_map<int, ScanReports> mScanReports;
  uint64_t scanReportsUntil();
  void forgetScanReport(int wd, const std::string &name);
  void expireScanReports();
  // Where a scan saw the directory of a node that is elsewhere in the tree, by the node's watch descriptor, see
  // sight(). And the nodes sighted in each node's directory, by watch descriptors too.
  std::map<int, Sighting> mSightings;
  std::map<int, std::set<int>> mSightedIn;
  std::vector<std::string> mExcludedPaths;
  std::unordered_set<std::string> mExcluded;
  ExcludedNames mExcludedNames;
  // A directory that excluded paths go through, as they spell it: the watched path or one below it. Followed on disk
  // name by name, see followWay(): whether it leads to a directory by watched directories only, to which one, and
  // the names it looked up on the way with the directory each was in.
  struct Way {
    std::string path;
    bool leads;
    Directory directory;
    std::vector<std::pair<Directory, std::string>> lookups;
  };
  // By their path. Followed once, and again when one of their lookups may give another answer, see lookupChanged():
  // asking which excluded paths lead to a directory takes no lookup on disk for each of them.
  std::unordered_map<std::string, Way> mWays;
  std::map<Directory, std::set<Way *>> mWaysTo;
  std::map<std::pair<Directory, std::string>, std::set<Way *>> mWaysByLookup;
  // What the paths of the nodes held at symlinks that know their own place look up, by their watch descriptors, see
  // traceLinkWay(). And the directories of the holders whose ways looked up an entry that changed, not settled yet.
  std::map<int, std::vector<Lookup>> mLinkWays;
  std::map<Lookup, std::set<int>> mLinkWaysByLookup;
  std::set<Directory> mLinksToSettle;
  // A way leads elsewhere, or the tree's own names changed, since revalidateRejected() last asked.
  bool mWaysChanged = false;
  // As many as the kernel follows in one path.
  static constexpr int MAX_LINKS_FOLLOWED = 40;
  // Plenty for a real path, few enough that a symlink spelling `a/../a/../` over and over costs little.
  static constexpr size_t MAX_LINK_LOOKUPS = 1024;
  // The names an excluded path names in each node's directory, directories or not as far as a scan could tell, to ask
  // about again once the answer may be another, see rejectsChild(). By the tree's own path, which only changes with
  // the tree.
  std::map<InotifyNode *, std::set<std::string>> mRejectedByPath;
  // Or by another path, which can stop leading there wherever something changes, see revalidateRejected().
  std::map<InotifyNode *, std::set<std::string>> mRejectedByAlias;
  // Or by the filter, see Rejected::BY_FILTER.
  std::map<InotifyNode *, std::set<std::string>> mRejectedByFilter;
  // The ones of those rejected for where they lead, or for their physical path, and whether a symlink changed since
  // they were asked about, see recheckFilteredLinks().
  std::map<InotifyNode *, std::set<std::string>> mFilteredLinks;
  bool mFilteredLinksChanged = false;
  // Nodes held at symlinks, each with the physical path its directory was last found at (empty if never resolved):
  // only a hint of where it is once the symlink dangles, see findStaleLinksByTarget(). searched once every excluded
  // directory was read for it at a dangling symlink, without finding it: only the ones that change are read for it
  // again, see excludeLinks().
  struct LinkNode {
    std::string target;
    bool searched = false;
  };
  std::map<InotifyNode *, LinkNode> mLinkNodes;
  // Symlink entries skipped because of their physical targets.
  std::map<InotifyNode *, std::set<std::string>> mRejectedLinks;
  // Resolved lazily and shared by the scans in a creation pass. Complete unless resolving an excluded path failed
  // for another reason than it not being there, or not being allowed.
  std::vector<PhysicalExclusion> mPhysicalExclusions;
  bool mPhysicalExclusionsLoaded = false;
  bool mPhysicalExclusionsComplete = true;
  bool mLinksChanged = false;
  // A filesystem event may have moved a directory below an excluded root: the excluded paths it may have arrived at,
  // or any of them, which the paths don't matter with. Both come with mLinksChanged, see revalidateLinks().
  std::set<std::string> mChangedLinkRoots;
  bool mAllLinkRootsChanged = false;
  // The skipped symlinks wait for the scans still pending, see includeLinks(), with the update that waits for them.
  bool mLinksAwaitScans = false;
  std::shared_ptr<UpdateCompletion> mLinkUpdate;
  bool mRootExcluded = false;
  // The directories it ignores are rejected like excluded paths, by the tree's own path, see exclusionOfChild(). Its
  // answers change with its patterns and ignore files too, see excludeFiltered() and includeFiltered().
  std::shared_ptr<PathFilter> mFilter;
  LinkFound mLinkFound;
  bool leadsToIgnored(const std::string &path, bool knownLink);
  std::string mWatchedPath;
  Directory mWatchedDirectory;

  friend class InotifyNode;
};

#endif
