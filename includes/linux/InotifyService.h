#ifndef INOTIFY_EVENT_HANDLER_H
#define INOTIFY_EVENT_HANDLER_H

#include "FileDescriptor.h"
#include "InotifyEventLoop.h"
#include "InotifyTree.h"
#include "../PathFilter.h"
#include "../Queue.h"
#include "../SymlinkIndex.h"
#include <memory>
#include <queue>
#include <map>

class InotifyEventLoop;
class InotifyTree;

class InotifyService {
public:
  // filter: what the tree doesn't watch, besides the excluded paths. symlinks: told about the symlinks the tree finds,
  // and where it reports their directories; null to follow none.
  InotifyService(std::shared_ptr<EventQueue> queue, std::string path, const std::vector<std::string> &excludedPaths,
    std::shared_ptr<PathFilter> filter, std::shared_ptr<SymlinkIndex> symlinks);

  std::string getError();
  bool hasErrored();
  bool isWatching();
  std::future<bool> updateExcludedPaths(const std::vector<std::string> &excludedPaths);
  void updateExcludedPatterns(const std::vector<std::string> &excludedPatterns);

  ~InotifyService();
private:
  void create(int wd, std::string name, bool fromInCreate);
  void createDirectory(int wd, std::string name, bool fromInCreate);
  void createDirectoryTree(std::string directoryTreePath);
  void dispatch(EventType action, int wd, std::string name, bool fromInCreate, PathFilter::Kind filterKind,
    PathFilter::Kind kind);
  void dispatchRename(int fromWd, std::string fromName, int toWd, std::string toName);
  EmitCreatedEvent emitCreated();
  void watchIgnored(int wd);
  bool isQuarantined(int wd, uint32_t mask);
  void entryDropped(int wd, std::string name);
  void startRead(size_t bytesRead);
  void finishRead(int eventsRead, size_t bytesRead);
  void modify(int wd, std::string name, bool isDirectory);
  void remove(int wd, std::string name, bool isDirectory);
  void removeDirectory(int wd);
  void forgetIncompleteScans();
  bool removeDroppedWatch(int wd);
  void removeDroppedWatches(int eventsRead);
  void retryWatches();
  void revalidateRejected();
  void revalidateLinks(bool unknownMoveDestination = false);
  void removeMovedDirectory(int wd, std::string name);
  void rename(int fromWd, std::string fromName, int toWd, std::string toName);
  void renameDirectory(int fromWd, std::string fromName, int toWd, std::string toName);
  void setError(const std::string &error);
  void noteEntry(int wd, const std::string &directory, const std::string &name, bool replaced);
  void noteMove(int fromWd, const std::string &fromPath, const std::string &fromName, int toWd,
    const std::string &toPath, const std::string &toName);
  void finishFilterPass();
  int settleFilterChange();
  void applyFilter(bool include);
  void takeFilterChanges();
  void wakeForFilterChange();

  std::shared_ptr<EventQueue> mQueue;
  std::shared_ptr<PathFilter> mFilter;
  std::shared_ptr<SymlinkIndex> mSymlinks;
  // The filter's generation committed last (see PathFilter::commit()), the one whose exclusions alone the tree
  // applied, the last one seen, when it was seen, and when the first one not committed was, see settleFilterChange().
  // And below which directories the answers changed since, or all of them.
  uint64_t mCommittedGeneration = 0;
  uint64_t mExcludedGeneration = 0;
  uint64_t mSeenGeneration = 0;
  std::chrono::steady_clock::time_point mLastFilterChange;
  std::chrono::steady_clock::time_point mFirstUnapplied;
  std::chrono::steady_clock::time_point mNextFailedReadRetry;
  std::chrono::steady_clock::time_point mLastExclusionPass;
  std::set<std::string> mPendingDirectories;
  bool mPendingAll = false;
  // How long the ignore files must stay unchanged before the tree drops what they ignore, and before it watches what
  // they no longer ignore, longer: a save that truncates and then writes, or a checkout that removes and then
  // creates, would otherwise have it watch everything they ignore for a moment. Each at most so long after the first
  // change, so a burst of changes can't put them off for long.
  static constexpr std::chrono::milliseconds EXCLUSION_QUIET_PERIOD = std::chrono::milliseconds(200);
  static constexpr std::chrono::milliseconds EXCLUSION_MAX_DELAY = std::chrono::milliseconds(1000);
  static constexpr std::chrono::milliseconds INCLUSION_QUIET_PERIOD = std::chrono::milliseconds(1000);
  static constexpr std::chrono::milliseconds INCLUSION_MAX_DELAY = std::chrono::milliseconds(5000);
  static constexpr std::chrono::milliseconds FAILED_READ_RETRY = std::chrono::milliseconds(1000);

  // Last, and in this order: members go in reverse, also when the constructor throws. The loop first, whose thread
  // uses all of the above and reads the descriptor, then the tree, which holds its number, then the descriptor, whose
  // close drops the watches.
  FileDescriptor mInotifyInstance;
  std::unique_ptr<InotifyTree> mTree;
  std::unique_ptr<InotifyEventLoop> mEventLoop;

  friend class InotifyEventLoop;
};

#endif
