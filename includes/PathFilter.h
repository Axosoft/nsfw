#ifndef NSFW_PATH_FILTER_H
#define NSFW_PATH_FILTER_H

#include <atomic>
#include <cstdint>
#include <map>
#include <set>
#include <mutex>
#include <string>
#include <vector>

#include "FilterPaths.h"
#include "GitignoreRules.h"

struct FilterOptions {
  // Glob patterns with .gitignore syntax, relative to the watched path. They take precedence over the ignore files.
  std::vector<std::string> excludedPatterns;
  // Whether the ignore files of the git repository the watched path is in apply: its .gitignore files, its
  // info/exclude and the user's excludes file (core.excludesFile).
  bool respectGitignore = false;
};

// Whether a path below the watched one is ignored, by the excluded patterns or by git's ignore files. Paths are
// absolute and spelled as the native watchers spell them, '\' separated on Windows. The watched path and its
// ancestors are never ignored, even where git would ignore them: they were asked for.
//
// The .gitignore of a directory is read the first time a path in it is asked about, and read again when
// entryChanged() reports a change to it. The ones above the watched path, an info/exclude outside it and the user's
// excludes file are read once.
//
// Used from the watcher threads and the workers that update exclusions, so every call locks.
class PathFilter {
public:
  using Kind = filterpaths::EntryKind;

  PathFilter(const std::string &watchedPath, const FilterOptions &options);

  // Whether any pattern or ignore file may apply. False makes every other call cheap.
  bool isActive() const { return mActive; }

  // Replaces the excluded patterns in force. A watcher that keeps answers, the Linux one, commits them once it applied
  // them, see commit(); the others right away.
  void setPatterns(const std::vector<std::string> &patterns);
  void commitPatterns();

  // The Linux tree keeps answers: from now on the rules it applied are kept next to the ones in force, see commit().
  void trackApplied() { mTracksApplied = true; }

  // The rules in force as of generation upTo become the ones the Linux tree applied, which it asks about together
  // until then, see isChildIgnored(): it never watches what either ignores. Rules that changed since stay to commit.
  // Returns upTo.
  uint64_t commit(uint64_t upTo);

  // Whether name in directory, there now, is ignored, or a directory it is in. When only rules for directories match
  // it, what it is decides: the Linux tree's DIRECTORY is trusted, else it is looked up, and remembered for its removal;
  // kind is the watcher's guess for one already gone, see judge().
  // tree: answer for the Linux tree, see isIgnoredBelowWatched().
  bool isIgnored(const std::string &directory, const std::string &name, Kind kind = Kind::UNKNOWN, bool tree = false);

  // Whether the removal of name in directory, or its rename away, is ignored: as its creation was, see judge().
  bool isRemovalIgnored(const std::string &directory, const std::string &name, Kind kind);

  // isIgnored(), which also tells whether an entry of another kind seen there before was reported though this one
  // isn't: a symlink retargeted by renaming another over it, or a file a directory replaced whose removal comes
  // later, judged by what replaced it. That removal then has to be reported.
  bool isReplacementIgnored(const std::string &directory, const std::string &name, Kind kind, bool &replacedReported);

  // What entries were when last seen, gone stale while events weren't judged (paused).
  void forgetSeenKinds();

  // Whether name in directory is ignored, for a caller that knows directory isn't.
  bool isChildIgnored(const std::string &directory, const std::string &name, bool isDirectory);

  // Takes a change of name in directory into account: an ignore file is read again, which bumps the generation if its
  // rules changed. replaced: the entry was created, removed or moved, so the rules read below its path are read again
  // on their next use, see forget().
  void entryChanged(const std::string &directory, const std::string &name, bool replaced);

  // Whether a change of name, replaced or not, can change any rules, for a caller with work to do before telling.
  bool mayChangeRules(const std::string &name, bool replaced) const;

  // Takes a move into account: the rules read below the old path go to the new one, with the rules the tree applied
  // there, to be read again; what was at the new path goes. An ignore file moved changes both directories' rules.
  void entryMoved(const std::string &fromDirectory, const std::string &fromName, const std::string &toDirectory,
    const std::string &toName);

  // Drops the rules read at name in directory and below it, for a directory gone, or no longer watched, or about to be
  // scanned afresh: nothing the tree holds was decided with them anymore. Ones that couldn't be read stay, so a failure
  // doesn't stand for "no rules" next time.
  void forget(const std::string &directory, const std::string &name);

  // Increased each time the rules in force from an ignore file change, whenever it is read again: the answers about
  // the paths below it may have changed.
  uint64_t generation() const { return mGeneration; }

  // Reads again the ignore files that couldn't be read (no descriptor left, a sharing violation), which may bump the
  // generation; true while some still can't be. A watcher that keeps answers, the Linux one, calls it now and then.
  void retryFailedReads();
  bool hasFailedReads() const { return mFailedReads; }

  // The directories, relative to the watched path, whose ignore file's rules changed since the last call, or all of
  // them when a change applies everywhere: below those only can the answers have changed.
  void takeChanges(std::vector<std::string> &directories, bool &all);

  // What the entry at name in directory is, UNKNOWN if it can't be looked up. Symlinks are followed, as the Linux
  // watcher follows them to directories.
  static Kind kindOf(const std::string &directory, const std::string &name);

  using Rules = GitignoreRules;

private:
  // The rules in force from an ignore file, and the ones the Linux tree applied, see commit(). stale: read again on the
  // next use, keeping these until it can be. failed: the last read failed. everRead: a read succeeded once.
  struct CachedRules {
    Rules rules;
    Rules applied;
    bool stale;
    bool failed;
    bool everRead;
    // The generation the rules in force last changed at.
    uint64_t changedAt = 0;
  };

  bool toRelative(const std::string &directory, std::string &relative) const;
  bool toEntry(const std::string &directory, const std::string &name, std::string &entry,
    bool *unresolved = NULL) const;
#if defined(_WIN32)
  bool expandShortNames(const std::string &relative, std::string &expanded) const;
#endif
  std::string toBase(const std::string &entry) const;
  std::string keyOf(const std::string &directory) const;
  void refresh(CachedRules &cached, const std::string &file, bool ignoreFile, const std::string *directory);
  const CachedRules &rulesOf(CachedRules &cached, const std::string &file);
  bool isInGitDirectory(const std::string &path) const;
  std::string comparable(const std::string &path) const;
  void dropBelow(const std::string &entry);
  void markStaleBelow(const std::string &entry);
  void moveBelow(const std::string &from, const std::string &to);
  void refreshIgnoreFileOf(const std::string &entry);
  void markInfoExcludeIfBelow(const std::string &directory, const std::string &name);
  bool splitTrailing(std::string &directory, std::string &name) const;
  bool judge(const std::string &directory, const std::string &name, Kind kind, bool gone, bool tree,
    bool *replacedReported);
  void dropSeenBelow(const std::string &key);
  void moveSeenBelow(const std::string &fromKey, const std::string &toKey);
  void seenMoved(const std::string &fromDirectory, const std::string &fromName, const std::string &toDirectory,
    const std::string &toName);
  bool isIgnoredBelowWatched(const std::string &relative, bool isDirectory, bool tree = false);
  bool isIgnoredBy(const std::string &relative, bool isDirectory, bool applied, bool conservative);
  const CachedRules &rulesOf(const std::string &directory);
  std::string ignoreFileOf(const std::string &directory) const;
  void loadRepository();

  std::atomic<bool> mActive{false};
  std::atomic<uint64_t> mGeneration{0};
  bool mCaseSensitive = true;

  // The watched path, '/' separated, and the directory the ignore files' paths are relative to: the root of the
  // repository with respectGitignore, the watched path otherwise.
  std::string mWatchedPath;
  std::string mBase;
  // The watched path relative to mBase, "" when they are the same.
  std::string mWatchedRelative;

  Rules mPatterns;
  bool mGitignore = false;
  CachedRules mExcludesFile = { Rules(), Rules(), false, false, true };
  CachedRules mInfoExclude = { Rules(), Rules(), false, false, true };
  Rules mAppliedPatterns;
  bool mTracksApplied = false;
  // Whether some applied rules differ from the ones in force, so the tree has two answers to ask for.
  bool mFilesDiverged = false;
  bool mPatternsDiverged = false;
  std::set<std::string> mChangedDirectories;
  bool mAllChanged = false;
  std::atomic<bool> mFailedReads{false};
  // '/' separated, empty if there is none to read.
  std::string mExcludesFilePath;
  std::string mInfoExcludePath;
  // The rules of the .gitignore in each directory read, by the key of its path relative to mBase ("" for mBase
  // itself), empty for one without. Ordered, so a directory's entries and the ones below it are a range.
  std::map<std::string, CachedRules> mDirectoryRules;
  // What the paths only rules for directories match were when last seen, by their keys, for when they are gone, see
  // judge(). Dropped and moved along with the rules below them, see dropBelow() and moveBelow().
  std::map<std::string, Kind> mSeenKinds;
  std::mutex mMutex;
};

#endif
