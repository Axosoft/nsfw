#ifndef NSFW_SYMLINK_INDEX_H
#define NSFW_SYMLINK_INDEX_H

#include <atomic>
#include <filesystem>
#include <functional>
#include <map>
#include <mutex>
#include <set>
#include <string>
#include <thread>
#include <vector>

// The symlinks below the watched path that lead to a file below it too, so a change of that file is also reported
// under each symlink: none of the watchers reports it there, since the symlink's own directory entry doesn't change.
//
// What it holds grows with the number of symlinks only. The watcher tells it about the ones it finds while it lists
// the tree, see linkFound(); the queue about every event, see eventHappened(), so the symlinks created, removed,
// retargeted or renamed afterwards are followed on every platform.
//
// A symlink is reported as what it leads to is: modified when its target is, deleted when its target is removed or
// renamed away (it dangles), created when its target appears. A chain of symlinks counts as a whole: each link in it
// changing counts as its target changing. A cycle, or a target that is a directory, outside the watched path or in
// an excluded path, is not followed.
//
// Paths are absolute and spelled as the native watchers spell them. Used from the watcher threads and the workers that
// update exclusions, so every call locks.
class SymlinkIndex {
public:
  enum class Change { CREATED, DELETED, MODIFIED };

  // A change to report under a symlink, and the file it leads to, which an ignored target makes ignored too.
  struct Derived {
    Change change;
    std::string directory, name;
    std::string targetDirectory, targetName;
  };

  // Whether name in directory is a directory not to look into.
  using IgnoredDirectory = std::function<bool(const std::string &directory, const std::string &name)>;
  // Sets reported to the path the watcher reports the real directory `directory` under, as of the events it told so
  // far; false to keep `directory`.
  using ReportedDirectory = std::function<bool(const std::string &directory, std::string &reported)>;

  // throughDirectoryLinks: the watcher follows symlinks to directories, so it may report a path in the tree through
  // one of them (the Linux tree holds a directory where it saw it first).
  SymlinkIndex(const std::string &watchedPath, const std::vector<std::string> &excludedPaths, bool caseSensitive,
    bool throughDirectoryLinks);
  ~SymlinkIndex();

  // Lists the watched tree once, in the background, for the symlinks already there, for a watcher that doesn't list
  // it itself. Skips the excluded directories and the ones ignored says to, and doesn't go through symlinks.
  void walk(IgnoredDirectory ignored);
  void stopWalk();

  void setExcludedPaths(const std::vector<std::string> &excludedPaths);

  // For a watcher that reports a directory through a symlink to it: the changes under a symlink in it are reported
  // where the watcher reports the symlink's own. Called from eventHappened() only, so it must be safe to call from
  // whatever thread tells events; nullptr to stop.
  void setReportedDirectory(ReportedDirectory reportedDirectory);

  // name in directory, which a watcher listing the tree found to be a symlink.
  void linkFound(const std::string &directory, const std::string &name);

  // Follows an event that happened at from (and to, for a rename) and appends what it means for the symlinks leading
  // there, in order. renamed: to is where from went, else to is empty.
  void eventHappened(Change change, bool renamed, const std::string &fromDirectory, const std::string &fromName,
    const std::string &toDirectory, const std::string &toName, std::vector<Derived> &derived);

private:
  struct Link {
    std::string key;
    std::string directory, name;
    // The keys of the paths it goes through after itself, the file it leads to last.
    std::vector<std::string> chain;
    // The keys of the symlinks to directories the paths it goes through are written through: changing them changes it.
    std::vector<std::string> through;
    // Spelled as the native watchers spell it.
    std::string targetDirectory, targetName;
    // It leads to a path below the watched one that isn't a directory, which may not exist.
    bool followed = false;
    // Whether what it leads to was last reported there: what its events have told, which the disk may be ahead of.
    // Only reporting changes it, but where nothing was reported yet, from what is on disk.
    bool shown = false;
    // It leads to a directory below the watched path, which the watcher may report paths through.
    bool treeDirectory = false;
  };

  void walkTree(const IgnoredDirectory &ignored);
  std::string keyOf(const std::string &path) const;
  std::string physical(const std::string &path) const;
  std::string pathKey(const std::string &path) const;
  void reconcile(const std::string &key, const std::set<std::string> &exact, const std::set<std::string> &below,
    Change change, bool renamedTo, bool plainDirectory, const std::set<std::string> &skipped,
    std::vector<Derived> &derived);
  std::string spelled(const std::filesystem::path &real) const;
  std::string nativeJoin(const std::string &directory, const std::string &name) const;
  bool isExcludedKey(const std::string &key) const;
  bool isBelowKey(const std::string &key, const std::string &ancestor) const;
  bool reported(const Link &link) const;
  std::string addLink(const std::string &directory, const std::string &name);
  void removeLink(const std::string &key);
  bool resolve(Link &link, bool &present);
  void dependOnLinksIn(const std::filesystem::path &written, Link &link) const;
  void index(const Link &link);
  void unindex(const Link &link);
  void linksThrough(const std::string &key, std::set<std::string> &exact, std::set<std::string> &below) const;
  std::vector<Link> takeLinksAtOrBelow(const std::string &key);
  void emit(Change change, const Link &link, std::vector<Derived> &derived) const;

  const bool mCaseSensitive;
  const bool mThroughDirectoryLinks;
  // The keys of the symlinks to directories in the tree.
  std::set<std::string> mTreeDirectoryLinks;
  // The watched path as the watcher was given it, '/' separated, and its key. Symlinks lead to real paths, which
  // are mapped below it through the key of its real path.
  std::string mNativeWatchedPath;
  std::string mWatchedPath;
  std::string mWatchedKey;
  std::string mRealWatchedKey;
  std::vector<std::string> mExcludedKeys;
  ReportedDirectory mReportedDirectory;
  // By the keys of their paths: comparable, '/' separated.
  std::map<std::string, Link> mLinks;
  // The keys of the symlinks each path in a chain is in, by that path's key. Ordered, so the paths below a directory
  // are a range.
  std::map<std::string, std::set<std::string>> mThrough;
  std::mutex mMutex;
  std::thread mWalk;
  std::atomic<bool> mStopWalk{false};
};

#endif
