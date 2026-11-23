#include "../includes/PathFilter.h"

#include <cctype>

#if defined(_WIN32)
#include <Windows.h>
#else
#include <unistd.h>
#endif

#include "../includes/GitRepository.h"

namespace {

const char *const IGNORE_FILE = ".gitignore";

using namespace filterpaths;

ReadResult readRules(const std::string &path, PathFilter::Rules &rules, bool ignoreFile = false) {
  std::string contents;
  const ReadResult result = readFile(path, contents, ignoreFile);
  rules = result == ReadResult::READ ? parseGitignoreRules(contents) : PathFilter::Rules();
  return result;
}

}

using namespace filterpaths;

PathFilter::PathFilter(const std::string &watchedPath, const FilterOptions &options) {
  mWatchedPath = withoutTrailingSeparators(toGeneric(watchedPath));
  mBase = mWatchedPath;
#if defined(_WIN32)
  mCaseSensitive = false;
#elif defined(__APPLE__)
  mCaseSensitive = pathconf(watchedPath.c_str(), _PC_CASE_SENSITIVE) > 0;
#endif
  if (options.respectGitignore) {
    loadRepository();
  }
  setPatterns(options.excludedPatterns);
  commitPatterns();
}

void PathFilter::setPatterns(const std::vector<std::string> &patterns) {
  Rules rules;
  for (const std::string &pattern : patterns) {
    Rules parsed = parseGitignoreRules(pattern);
    rules.insert(rules.end(), parsed.begin(), parsed.end());
  }
  std::lock_guard<std::mutex> lock(mMutex);
  mPatterns = std::move(rules);
  mPatternsDiverged = mTracksApplied && mPatterns != mAppliedPatterns;
  // The applied patterns still count until committed.
  mActive = !mPatterns.empty() || !mAppliedPatterns.empty() || mGitignore;
}

void PathFilter::commitPatterns() {
  std::lock_guard<std::mutex> lock(mMutex);
  mAppliedPatterns = mPatterns;
  mPatternsDiverged = false;
  mActive = !mPatterns.empty() || mGitignore;
}

uint64_t PathFilter::commit(uint64_t upTo) {
  std::lock_guard<std::mutex> lock(mMutex);
  bool diverged = false;
  auto commitRules = [upTo, &diverged](CachedRules &cached) {
    if (cached.changedAt <= upTo) {
      cached.applied = cached.rules;
    } else {
      diverged = true;
    }
  };
  for (auto &cached : mDirectoryRules) {
    commitRules(cached.second);
  }
  commitRules(mInfoExclude);
  commitRules(mExcludesFile);
  mFilesDiverged = diverged;
  mAppliedPatterns = mPatterns;
  mPatternsDiverged = false;
  mActive = !mPatterns.empty() || mGitignore;
  return upTo;
}

// Reads the ignore files of the repository the watched path is in that don't change with the paths below it, see
// findGitRepository(). Outside a trusted repository the ignore files don't apply.
void PathFilter::loadRepository() {
  GitRepository repository;
  if (!findGitRepository(mWatchedPath, repository)) {
    return;
  }
  mGitignore = true;
  mBase = repository.root;
  mWatchedRelative = mWatchedPath.size() > mBase.size() ? mWatchedPath.substr(join(mBase, "").size()) : "";
  auto readOnce = [this](CachedRules &cached, const std::string &path) {
    cached.failed = cached.stale = readRules(fromGeneric(path), cached.rules) == ReadResult::FAILED;
    cached.everRead = !cached.failed;
    cached.applied = cached.rules;
    mFailedReads = mFailedReads || cached.failed;
  };
  mInfoExcludePath = repository.infoExcludePath;
  if (!mInfoExcludePath.empty()) {
    readOnce(mInfoExclude, mInfoExcludePath);
  }
  mExcludesFilePath = repository.excludesFilePath;
  if (!mExcludesFilePath.empty()) {
    readOnce(mExcludesFile, mExcludesFilePath);
  }
}

// The native path of the .gitignore in directory, relative to mBase.
std::string PathFilter::ignoreFileOf(const std::string &directory) const {
  return fromGeneric(join(directory.empty() ? mBase : join(mBase, directory), IGNORE_FILE));
}

// directory relative to the watched path, "" for the watched path itself; false if it isn't below it.
bool PathFilter::toRelative(const std::string &directory, std::string &relative) const {
  const std::string path = withoutTrailingSeparators(toGeneric(directory));
  if (!startsWith(path, mWatchedPath, mCaseSensitive)) {
    return false;
  }
  if (path.size() == mWatchedPath.size()) {
    relative.clear();
    return true;
  }
  const bool rootWithSeparator = mWatchedPath.back() == '/';
  if (!rootWithSeparator && path[mWatchedPath.size()] != '/') {
    return false;
  }
  relative = path.substr(mWatchedPath.size() + (rootWithSeparator ? 0 : 1));
  return true;
}

#if defined(_WIN32)
// Whether a name may be an 8.3 short name, like "NODE_M~1".
static bool mayBeShortName(const std::string &path) {
  for (size_t tilde = path.find('~'); tilde != std::string::npos; tilde = path.find('~', tilde + 1)) {
    if (tilde + 1 < path.size() && isdigit((unsigned char)path[tilde + 1])) {
      return true;
    }
  }
  return false;
}
#endif

#if defined(_WIN32)
// The long form of a path below the watched one, relative to it, false if it can't be had (it is gone, say).
bool PathFilter::expandShortNames(const std::string &relative, std::string &expanded) const {
  const std::wstring shortPath = toWide(fromGeneric(join(mWatchedPath, relative)));
  const DWORD length = GetLongPathNameW(shortPath.c_str(), NULL, 0);
  if (length == 0) {
    return false;
  }
  std::wstring longPath(length, L'\0');
  const DWORD written = GetLongPathNameW(shortPath.c_str(), &longPath[0], length);
  if (written == 0 || written >= length) {
    return false;
  }
  longPath.resize(written);
  return toRelative(fromWide(longPath), expanded) && !expanded.empty();
}
#endif

// name in directory relative to the watched path, false if directory isn't below it. Windows can report a name, or a
// directory it is in, in its 8.3 form, which the rules don't match and the cache would keep apart: that is expanded,
// the directory alone once the name is gone. unresolved, if given: whether the name may still be a short one.
bool PathFilter::toEntry(const std::string &directory, const std::string &name, std::string &entry,
  bool *unresolved) const {
  std::string relative;
  if (unresolved != NULL) {
    *unresolved = false;
  }
  if (name.empty()) {
    std::string parent = directory;
    std::string last;
    return splitTrailing(parent, last) && toEntry(parent, last, entry, unresolved);
  }
  if (!toRelative(directory, relative)) {
    return false;
  }
  entry = relative.empty() ? name : relative + "/" + name;
#if defined(_WIN32)
  if (mayBeShortName(entry)) {
    std::string expanded;
    if (expandShortNames(entry, expanded)) {
      entry = expanded;
    } else {
      if (!relative.empty() && mayBeShortName(relative) && expandShortNames(relative, expanded)) {
        entry = expanded + "/" + name;
      }
      if (unresolved != NULL) {
        *unresolved = mayBeShortName(entry);
      }
    }
  }
#endif
  return true;
}

// An entry relative to the watched path, relative to mBase.
std::string PathFilter::toBase(const std::string &entry) const {
  return mWatchedRelative.empty() ? entry : (entry.empty() ? mWatchedRelative : mWatchedRelative + "/" + entry);
}

// The key of a directory in the rules cache: one per directory, however a case-insensitive filesystem spells it.
std::string PathFilter::keyOf(const std::string &directory) const {
  return mCaseSensitive ? directory : lower(directory);
}

// Reads the rules again, bumping the generation when they differ from the ones in force. One that can't be read
// keeps the ones in force, and stays stale to be read again on the next use, and by retryFailedReads().
// directory: the one whose .gitignore it is, relative to mBase; NULL for a file whose rules apply everywhere. A file
// read for the first time after failing counts as changed: the Linux tree took it for one ignoring everything.
void PathFilter::refresh(CachedRules &cached, const std::string &file, bool ignoreFile, const std::string *directory) {
  Rules rules;
  if (readRules(file, rules, ignoreFile) == ReadResult::FAILED) {
    cached.stale = cached.failed = true;
    mFailedReads = true;
    return;
  }
  if (rules != cached.rules || !cached.everRead) {
    cached.rules = std::move(rules);
    cached.changedAt = ++mGeneration;
    if (mTracksApplied) {
      mFilesDiverged = true;
      if (directory != NULL) {
        mChangedDirectories.insert(*directory);
      } else {
        mAllChanged = true;
      }
    }
  }
  cached.stale = cached.failed = false;
  cached.everRead = true;
}

// A directory never read took part in no decision, so reading it for the first time changes none: the generation
// stays. Rules a decision the tree still holds used are never dropped, only made stale or moved along, see
// entryMoved(); forget() drops them only once nothing below is watched. A first read that fails stands for no rules
// (or, for the tree, for ignoring every directory below) until a retry.
const PathFilter::CachedRules &PathFilter::rulesOf(const std::string &directory) {
  const std::string key = keyOf(directory);
  auto found = mDirectoryRules.find(key);
  if (found == mDirectoryRules.end()) {
    Rules rules;
    const bool failed = readRules(ignoreFileOf(directory), rules, true) == ReadResult::FAILED;
    mFailedReads = mFailedReads || failed;
    // Read for the first time: nothing applied other rules here.
    Rules applied = mTracksApplied ? rules : Rules();
    return mDirectoryRules.emplace(key, CachedRules { std::move(rules), std::move(applied), failed, failed, !failed })
      .first->second;
  }
  if (found->second.stale) {
    refresh(found->second, ignoreFileOf(directory), true, &directory);
  }
  return found->second;
}

void PathFilter::retryFailedReads() {
  if (!mFailedReads) {
    return;
  }
  std::lock_guard<std::mutex> lock(mMutex);
  bool failed = false;
  for (auto &cached : mDirectoryRules) {
    if (cached.second.failed) {
      // Keys are spelled like the paths they come from, or lower-cased where case doesn't matter.
      refresh(cached.second, ignoreFileOf(cached.first), true, &cached.first);
      failed = failed || cached.second.failed;
    }
  }
  if (mInfoExclude.failed) {
    refresh(mInfoExclude, fromGeneric(mInfoExcludePath), false, NULL);
    failed = failed || mInfoExclude.failed;
  }
  if (mExcludesFile.failed) {
    refresh(mExcludesFile, fromGeneric(mExcludesFilePath), false, NULL);
    failed = failed || mExcludesFile.failed;
  }
  mFailedReads = failed;
}

const PathFilter::CachedRules &PathFilter::rulesOf(CachedRules &cached, const std::string &file) {
  if (cached.stale) {
    refresh(cached, fromGeneric(file), false, NULL);
  }
  return cached;
}

void PathFilter::takeChanges(std::vector<std::string> &directories, bool &all) {
  std::lock_guard<std::mutex> lock(mMutex);
  all = mAllChanged;
  for (const std::string &directory : mChangedDirectories) {
    if (mWatchedRelative.empty()) {
      directories.push_back(directory);
    } else if (directory == mWatchedRelative) {
      directories.push_back("");
    } else if (directory.compare(0, mWatchedRelative.size() + 1, mWatchedRelative + "/") == 0) {
      directories.push_back(directory.substr(mWatchedRelative.size() + 1));
    } else {
      // Above the watched path: it applies to all of it.
      all = true;
    }
  }
  mChangedDirectories.clear();
  mAllChanged = false;
}

// Whether a path relative to mBase is a .git directory or below one, where git never applies its ignore files.
bool PathFilter::isInGitDirectory(const std::string &path) const {
  for (size_t start = 0; start <= path.size();) {
    size_t end = path.find('/', start);
    if (end == std::string::npos) end = path.size();
    if (end - start == 4 && (mCaseSensitive ? path.compare(start, 4, ".git") == 0 :
      lower(path.substr(start, 4)) == ".git")) {
      return true;
    }
    start = end + 1;
  }
  return false;
}

// Only the path itself: whether one of the directories it is in is ignored is the caller's to ask. relative: to the
// watched path, not empty. tree: for a watcher that watches what isn't ignored, the Linux one: ignored by the rules it
// applied or by the ones in force (they differ until it commits them, see commit()), and an ignore file that never
// could be read ignores every directory below it rather than none.
bool PathFilter::isIgnoredBelowWatched(const std::string &relative, bool isDirectory, bool tree) {
  if (!tree) {
    return isIgnoredBy(relative, isDirectory, false, false);
  }
  if (!mTracksApplied || (!mFilesDiverged && !mPatternsDiverged)) {
    return isIgnoredBy(relative, isDirectory, false, isDirectory);
  }
  return isIgnoredBy(relative, isDirectory, false, isDirectory) || isIgnoredBy(relative, isDirectory, true, isDirectory);
}

// applied: by the rules the tree applied rather than the ones in force.
bool PathFilter::isIgnoredBy(const std::string &relative, bool isDirectory, bool applied, bool conservative) {
  GitignoreMatch match = matchGitignoreRules(applied ? mAppliedPatterns : mPatterns, relative, isDirectory, mCaseSensitive);
  const std::string path = toBase(relative);
  if (match != GitignoreMatch::NONE || !mGitignore || isInGitDirectory(path)) {
    return match == GitignoreMatch::IGNORED;
  }
  // A .gitignore closer to the path takes precedence over the ones above it.
  for (size_t slash = path.rfind('/');; slash = path.rfind('/', slash - 1)) {
    const bool top = slash == std::string::npos;
    const CachedRules &cached = rulesOf(top ? "" : path.substr(0, slash));
    if (conservative && cached.failed && !cached.everRead) {
      return true;
    }
    match = matchGitignoreRules(applied ? cached.applied : cached.rules, top ? path : path.substr(slash + 1), isDirectory,
      mCaseSensitive);
    if (match != GitignoreMatch::NONE) {
      return match == GitignoreMatch::IGNORED;
    }
    if (top || slash == 0) {
      break;
    }
  }
  for (auto *file : { &mInfoExclude, &mExcludesFile }) {
    const std::string &filePath = file == &mInfoExclude ? mInfoExcludePath : mExcludesFilePath;
    if (match != GitignoreMatch::NONE || filePath.empty()) {
      continue;
    }
    const CachedRules &cached = rulesOf(*file, filePath);
    if (conservative && cached.failed && !cached.everRead) {
      return true;
    }
    match = matchGitignoreRules(applied ? cached.applied : cached.rules, path, isDirectory, mCaseSensitive);
  }
  return match == GitignoreMatch::IGNORED;
}

bool PathFilter::isIgnored(const std::string &directory, const std::string &name, Kind kind, bool tree) {
  return judge(directory, name, kind, false, tree, NULL);
}

bool PathFilter::isRemovalIgnored(const std::string &directory, const std::string &name, Kind kind) {
  return judge(directory, name, kind, true, false, NULL);
}

bool PathFilter::isReplacementIgnored(const std::string &directory, const std::string &name, Kind kind,
  bool &replacedReported) {
  replacedReported = false;
  return judge(directory, name, kind, false, false, &replacedReported);
}

bool PathFilter::judge(const std::string &directory, const std::string &name, Kind kind, bool gone, bool tree,
  bool *replacedReported) {
  std::string path;
  if (!mActive || !toEntry(directory, name, path)) {
    return false;
  }
  std::lock_guard<std::mutex> lock(mMutex);
  // Nothing in an ignored directory can be included again, as in git.
  for (size_t slash = path.find('/'); slash != std::string::npos; slash = path.find('/', slash + 1)) {
    if (isIgnoredBelowWatched(path.substr(0, slash), true, tree)) {
      return true;
    }
  }
  const bool asFile = isIgnoredBelowWatched(path, false, tree);
  const bool asDirectory = isIgnoredBelowWatched(path, true, tree);
  if (asFile == asDirectory) {
    return asFile;
  }
  // Only rules for directories match it: what it is decides. The Linux tree only asks about a directory it holds or
  // one a symlink leads to.
  if (tree && kind == Kind::DIRECTORY) {
    return asDirectory;
  }
  const std::string key = keyOf(toBase(path));
  const auto seen = mSeenKinds.find(key);
  if (!gone) {
    // There, as it is now, following a symlink as the watcher does.
    const Kind actual = kindOf(directory, name);
    if (actual != Kind::UNKNOWN) {
      // Seen as another kind: replaced since, its removal maybe not handled yet, or never announced (a rename over it).
      if (replacedReported != NULL && seen != mSeenKinds.end() && seen->second != actual) {
        *replacedReported = !(seen->second == Kind::DIRECTORY ? asDirectory : asFile);
      }
      mSeenKinds[key] = actual;
      return actual == Kind::DIRECTORY ? asDirectory : asFile;
    }
    // Already gone: as below, kept for its removal, which then gets the same answer.
    const Kind was = seen != mSeenKinds.end() ? seen->second : kind == Kind::FILE ? Kind::FILE : Kind::DIRECTORY;
    mSeenKinds[key] = was;
    return was == Kind::FILE ? asFile : asDirectory;
  }
  // Gone: judged as it was last seen, which is how its creation was judged. A stat now would find whatever replaced
  // it since, and a watcher reports a removed symlink to a directory as no directory.
  // Never seen: as the watcher says, else (Windows doesn't tell) as a directory, since a file is rarely named like a
  // pattern for directories (build/) while a removed node_modules often was there before watching.
  Kind was = kind;
  if (seen != mSeenKinds.end()) {
    was = seen->second;
    mSeenKinds.erase(seen);
  }
  return was == Kind::FILE ? asFile : asDirectory;
}

bool PathFilter::isChildIgnored(const std::string &directory, const std::string &name, bool isDirectory) {
  std::string path;
  if (!mActive || !toEntry(directory, name, path)) {
    return false;
  }
  std::lock_guard<std::mutex> lock(mMutex);
  if (mFilesDiverged || mPatternsDiverged) {
    // A directory the new rules ignore stays watched until they are applied: what is created in it meanwhile isn't.
    for (size_t slash = path.find('/'); slash != std::string::npos; slash = path.find('/', slash + 1)) {
      if (isIgnoredBelowWatched(path.substr(0, slash), true, true)) {
        return true;
      }
    }
  }
  return isIgnoredBelowWatched(path, isDirectory, true);
}

// The form two spellings of one path share: without the "\\?\" prefix the paths of the watched directory have and
// one read from a .git file may not, and in lower case where the filesystem ignores case.
std::string PathFilter::comparable(const std::string &path) const {
  return filterpaths::comparable(path, mCaseSensitive);
}

// macOS reports a path with a trailing separator, which its watcher splits into the path and no name: the directory's
// own name and its parent instead. False for a root.
bool PathFilter::splitTrailing(std::string &directory, std::string &name) const {
  const std::string path = withoutTrailingSeparators(toGeneric(directory));
  const size_t root = rootLength(path);
  const size_t slash = path.rfind('/');
  if (slash == std::string::npos || path.size() <= root) {
    return false;
  }
  directory = slash < root ? path.substr(0, root) : path.substr(0, slash);
  name = path.substr(slash + 1);
  return true;
}

bool PathFilter::mayChangeRules(const std::string &name, bool replaced) const {
  bool maybeIgnoreFile = mCaseSensitive ? name == IGNORE_FILE : lower(name) == IGNORE_FILE;
#if defined(_WIN32)
  // Windows may report it by its short name, which only toEntry() expands.
  maybeIgnoreFile = maybeIgnoreFile || mayBeShortName(name);
#endif
  return mGitignore && (replaced || maybeIgnoreFile || name == "exclude");
}

void PathFilter::entryChanged(const std::string &spelledDirectory, const std::string &spelledName, bool replaced) {
  if (!mGitignore) {
    if (replaced) seenMoved(spelledDirectory, spelledName, "", "");
    return;
  }
  std::string directory = spelledDirectory;
  std::string name = spelledName;
  if (name.empty() && !splitTrailing(directory, name)) {
    // A root, which macOS reports as itself with no name when it dropped events: an ancestor of the watched path.
    if (replaced) {
      std::lock_guard<std::mutex> lock(mMutex);
      markStaleBelow("");
    }
    return;
  }
  if (!mayChangeRules(name, replaced)) {
    return;
  }
  std::lock_guard<std::mutex> lock(mMutex);
  if (!mInfoExcludePath.empty() &&
    comparable(join(withoutTrailingSeparators(toGeneric(directory)), name)) == comparable(mInfoExcludePath)) {
    refresh(mInfoExclude, fromGeneric(mInfoExcludePath), false, NULL);
    return;
  }

  std::string entry;
  bool unresolved = false;
  if (!toEntry(directory, name, entry, &unresolved)) {
    // The watched path itself, or one of its ancestors: macOS reports one when it dropped events, which may have been
    // changes to any ignore file.
    const std::string path = comparable(join(withoutTrailingSeparators(toGeneric(directory)), name));
    const std::string watched = comparable(mWatchedPath);
    if (replaced && watched.compare(0, path.size(), path) == 0 &&
      (watched.size() == path.size() || watched[path.size()] == '/' || path.back() == '/')) {
      markStaleBelow("");
    }
    return;
  }
  entry = toBase(entry);
  if (unresolved) {
    // A short name whose long form is gone: it may have been the .gitignore, or a directory with rules below it, below
    // the last directory whose name is known for sure. What is there may still be watched: kept, to be read again.
    const size_t slash = entry.rfind('/');
    const std::string parent = slash == std::string::npos ? "" : entry.substr(0, slash);
    size_t known = 0;
    for (size_t start = 0; start < parent.size();) {
      size_t end = parent.find('/', start);
      if (end == std::string::npos) end = parent.size();
      if (parent.find('~', start) < end) break;
      known = end;
      start = end + 1;
    }
    markStaleBelow(parent.substr(0, known));
    return;
  }
  refreshIgnoreFileOf(entry);
  if (replaced) {
    // Even one named .gitignore may be a directory.
    dropBelow(entry);
    markInfoExcludeIfBelow(directory, name);
  }
}

// When entry, relative to mBase, is a .gitignore: its directory's rules are read again.
void PathFilter::refreshIgnoreFileOf(const std::string &entry) {
  const size_t slash = entry.rfind('/');
  const std::string entryName = slash == std::string::npos ? entry : entry.substr(slash + 1);
  if (mCaseSensitive ? entryName != IGNORE_FILE : lower(entryName) != IGNORE_FILE) {
    return;
  }
  const std::string parent = slash == std::string::npos ? "" : entry.substr(0, slash);
  auto found = mDirectoryRules.find(keyOf(parent));
  if (found == mDirectoryRules.end()) {
    // Read for the first time: see rulesOf().
    rulesOf(parent);
  } else {
    refresh(found->second, ignoreFileOf(parent), true, &parent);
  }
}

// One that holds info/exclude, the .git or its info, went or came.
void PathFilter::markInfoExcludeIfBelow(const std::string &directory, const std::string &name) {
  if (mInfoExcludePath.empty()) {
    return;
  }
  const std::string path = comparable(join(withoutTrailingSeparators(toGeneric(directory)), name)) + "/";
  if (comparable(mInfoExcludePath).compare(0, path.size(), path) == 0) {
    mInfoExclude.stale = true;
  }
}

void PathFilter::entryMoved(const std::string &fromDirectory, const std::string &fromName,
  const std::string &toDirectory, const std::string &toName) {
  if (!mGitignore) {
    seenMoved(fromDirectory, fromName, toDirectory, toName);
    return;
  }
  std::string from;
  std::string to;
  bool fromUnresolved = false;
  bool toUnresolved = false;
  if (!toEntry(fromDirectory, fromName, from, &fromUnresolved) || !toEntry(toDirectory, toName, to, &toUnresolved) ||
    fromUnresolved || toUnresolved) {
    // Not both below the watched path, or a short name gone: a removal and a creation.
    entryChanged(fromDirectory, fromName, true);
    entryChanged(toDirectory, toName, true);
    return;
  }
  std::lock_guard<std::mutex> lock(mMutex);
  // info/exclude saved by writing another file and renaming it over.
  for (const auto *side : { &fromDirectory, &toDirectory }) {
    const std::string &sideName = side == &fromDirectory ? fromName : toName;
    if (!mInfoExcludePath.empty() &&
      comparable(join(withoutTrailingSeparators(toGeneric(*side)), sideName)) == comparable(mInfoExcludePath)) {
      refresh(mInfoExclude, fromGeneric(mInfoExcludePath), false, NULL);
    }
  }
  from = toBase(from);
  to = toBase(to);
  dropBelow(to);
  moveBelow(from, to);
  refreshIgnoreFileOf(from);
  refreshIgnoreFileOf(to);
  markInfoExcludeIfBelow(fromDirectory, fromName);
  markInfoExcludeIfBelow(toDirectory, toName);
}

void PathFilter::forget(const std::string &directory, const std::string &name) {
  std::string entry;
  if (!mGitignore || !toEntry(directory, name, entry)) {
    return;
  }
  std::lock_guard<std::mutex> lock(mMutex);
  dropBelow(toBase(entry));
}

// The rules read at entry and below it, from "entry/" up to "entry0", '0' following '/', all of them for "", go.
void PathFilter::dropBelow(const std::string &entry) {
  const std::string key = keyOf(entry);
  auto drop = [this](std::map<std::string, CachedRules>::iterator cached) {
    if (cached->second.failed) {
      cached->second.stale = true;
      return std::next(cached);
    }
    return mDirectoryRules.erase(cached);
  };
  auto begin = key.empty() ? mDirectoryRules.begin() : mDirectoryRules.find(key);
  if (!key.empty() && begin != mDirectoryRules.end()) {
    drop(begin);
  }
  for (auto below = key.empty() ? mDirectoryRules.begin() : mDirectoryRules.lower_bound(key + "/");
    below != mDirectoryRules.end() && (key.empty() || below->first < key + "0");) {
    below = drop(below);
  }
  dropSeenBelow(key);
}

// What was seen below the entry at key went with it; the entry itself stays until its own removal is judged, see
// judge().
void PathFilter::dropSeenBelow(const std::string &key) {
  if (key.empty()) {
    mSeenKinds.clear();
  } else {
    mSeenKinds.erase(mSeenKinds.lower_bound(key + "/"), mSeenKinds.lower_bound(key + "0"));
  }
}

// What was seen below the entry at fromKey moves to toKey; the entry itself is judged at both ends of its rename, see
// judge().
void PathFilter::moveSeenBelow(const std::string &fromKey, const std::string &toKey) {
  std::vector<std::pair<std::string, Kind>> seen;
  for (auto entry = mSeenKinds.lower_bound(fromKey + "/"); entry != mSeenKinds.end() && entry->first < fromKey + "0";) {
    seen.push_back({ toKey + entry->first.substr(fromKey.size()), entry->second });
    entry = mSeenKinds.erase(entry);
  }
  mSeenKinds.insert(seen.begin(), seen.end());
}

// Without ignore files to follow, entries are noted only for what was seen below them.
void PathFilter::seenMoved(const std::string &fromDirectory, const std::string &fromName,
  const std::string &toDirectory, const std::string &toName) {
  std::lock_guard<std::mutex> lock(mMutex);
  if (mSeenKinds.empty()) {
    return;
  }
  std::string from;
  std::string to;
  // A directory still there kept what is below it: macOS reports a directory whose attributes changed as replaced.
  const bool known = toEntry(fromDirectory, fromName, from) &&
    (!toDirectory.empty() || kindOf(fromDirectory, fromName) != Kind::DIRECTORY);
  const bool arrived = toEntry(toDirectory, toName, to);
  if (arrived) dropSeenBelow(keyOf(toBase(to)));
  if (known && arrived) {
    moveSeenBelow(keyOf(toBase(from)), keyOf(toBase(to)));
  } else if (known) {
    dropSeenBelow(keyOf(toBase(from)));
  }
}

void PathFilter::forgetSeenKinds() {
  std::lock_guard<std::mutex> lock(mMutex);
  mSeenKinds.clear();
}

// What is below entry may still be watched: its rules are only read again, keeping the applied ones.
void PathFilter::markStaleBelow(const std::string &entry) {
  const std::string key = keyOf(entry);
  for (auto &cached : mDirectoryRules) {
    if (key.empty() || cached.first == key || cached.first.compare(0, key.size() + 1, key + "/") == 0) {
      cached.second.stale = true;
    }
  }
  if (key.empty()) {
    mInfoExclude.stale = mExcludesFile.stale = true;
  }
}

// The directory at from now is at to, rules and applied rules along, to be read again there.
void PathFilter::moveBelow(const std::string &from, const std::string &to) {
  const std::string fromKey = keyOf(from);
  const std::string toKey = keyOf(to);
  std::vector<std::pair<std::string, CachedRules>> moved;
  // Keys like "from-x" sort between "from" and "from/": skipped.
  for (auto cached = mDirectoryRules.lower_bound(fromKey);
    cached != mDirectoryRules.end() && cached->first < fromKey + "0";) {
    if (cached->first != fromKey && cached->first.compare(0, fromKey.size() + 1, fromKey + "/") != 0) {
      ++cached;
      continue;
    }
    moved.push_back({ toKey + cached->first.substr(fromKey.size()), std::move(cached->second) });
    cached = mDirectoryRules.erase(cached);
  }
  for (auto &entry : moved) {
    entry.second.stale = true;
    mDirectoryRules[entry.first] = std::move(entry.second);
  }
  moveSeenBelow(fromKey, toKey);
}

PathFilter::Kind PathFilter::kindOf(const std::string &directory, const std::string &name) {
  return lookUp(fromGeneric(join(toGeneric(directory), name)));
}
