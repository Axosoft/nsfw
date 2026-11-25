#include "../includes/SymlinkIndex.h"

#include <filesystem>
#include <iterator>
#include <system_error>

#include "../includes/FilterPaths.h"

#if defined(_WIN32)
#include <Windows.h>
#endif

#pragma unmanaged

namespace fs = std::filesystem;
using namespace filterpaths;

namespace {

// As the kernels' own limit on the symlinks one lookup follows: a chain longer than that is a cycle.
const int MAX_HOPS = 40;

#if defined(_WIN32)
const char SEPARATOR = '\\';

fs::path toPath(const std::string &path) {
  return fs::path(toWide(path));
}

std::string fromPath(const fs::path &path) {
  return fromWide(path.native());
}
#else
const char SEPARATOR = '/';

fs::path toPath(const std::string &path) {
  return fs::path(path);
}

std::string fromPath(const fs::path &path) {
  return path.native();
}
#endif

// Listing a directory a cloud provider keeps online only would have it fetched: the Windows watcher doesn't either.
bool isRecalledOnAccess(const fs::path &directory) {
#if defined(_WIN32)
  const DWORD attributes = GetFileAttributesW(directory.c_str());
  return attributes != INVALID_FILE_ATTRIBUTES &&
    (attributes & (FILE_ATTRIBUTE_RECALL_ON_OPEN | FILE_ATTRIBUTE_RECALL_ON_DATA_ACCESS)) != 0;
#else
  (void)directory;
  return false;
#endif
}

// Whether the entry at path is there under a name spelled otherwise, which a case-insensitive filesystem matches.
bool isNamedOtherwise(const std::string &path) {
#if defined(_WIN32)
  WIN32_FIND_DATAW data;
  const std::wstring wide = toWide(path);
  HANDLE find = FindFirstFileW(wide.c_str(), &data);
  if (find == INVALID_HANDLE_VALUE) return false;
  FindClose(find);
  const size_t separator = wide.find_last_of(L"\\/");
  return wide.compare(separator == std::wstring::npos ? 0 : separator + 1, std::wstring::npos, data.cFileName) != 0;
#else
  (void)path;
  return false;
#endif
}

// What path is once the directory it is in is resolved; the name it ends with is kept, since it may be a symlink to
// follow, or not exist yet.
fs::path realParent(const fs::path &path) {
  std::error_code error;
  const fs::path name = path.filename();
  if (name.empty() || name == "." || name == "..") {
    fs::path resolved = fs::weakly_canonical(path, error);
    return error ? path : resolved;
  }
  fs::path parent = fs::weakly_canonical(path.parent_path(), error);
  return error ? path : parent / name;
}

}

SymlinkIndex::SymlinkIndex(const std::string &watchedPath, const std::vector<std::string> &excludedPaths,
  bool caseSensitive, bool throughDirectoryLinks):
  mCaseSensitive(caseSensitive),
  mThroughDirectoryLinks(throughDirectoryLinks) {
  mNativeWatchedPath = watchedPath;
  mWatchedPath = withoutTrailingSeparators(toGeneric(watchedPath));
  std::error_code error;
  const fs::path real = fs::weakly_canonical(toPath(watchedPath), error);
  mWatchedKey = comparable(mWatchedPath, mCaseSensitive);
  mRealWatchedKey = error ? mWatchedKey :
    comparable(withoutTrailingSeparators(toGeneric(fromPath(real))), mCaseSensitive);
  setExcludedPaths(excludedPaths);
}

SymlinkIndex::~SymlinkIndex() {
  stopWalk();
}

void SymlinkIndex::walk(IgnoredDirectory ignored) {
  mWalk = std::thread([this, ignored = std::move(ignored)]() {
    // Best effort: a failure only leaves the symlinks it didn't reach to be found by their events, and an exception
    // escaping the thread would end the process.
    try {
      walkTree(ignored);
    } catch (...) {
    }
  });
}

void SymlinkIndex::stopWalk() {
  mStopWalk = true;
  if (mWalk.joinable()) mWalk.join();
}

// One directory at a time, so one that can't be listed, or goes away meanwhile, only skips what is below it.
void SymlinkIndex::walkTree(const IgnoredDirectory &ignored) {
  std::vector<fs::path> pending = { toPath(mNativeWatchedPath) };
  while (!pending.empty() && !mStopWalk) {
    const fs::path directory = pending.back();
    pending.pop_back();
    const std::string nativeDirectory = fromPath(directory);
    std::error_code error;
    fs::directory_iterator entries(directory, fs::directory_options::skip_permission_denied, error);
    for (; !error && entries != fs::directory_iterator() && !mStopWalk; entries.increment(error)) {
      const fs::directory_entry &entry = *entries;
      const std::string name = fromPath(entry.path().filename());
      // Not followed: a symlink to a directory, or a junction, can lead out of the tree or back into it, endlessly.
      std::error_code entryError;
      const fs::file_status status = entry.symlink_status(entryError);
      if (entryError) continue;
      if (fs::is_symlink(status)) {
        linkFound(nativeDirectory, name);
      } else if (status.type() == fs::file_type::directory && !isRecalledOnAccess(entry.path())) {
        bool excluded;
        {
          std::lock_guard<std::mutex> lock(mMutex);
          excluded = isExcludedKey(keyOf(nativeJoin(nativeDirectory, name)));
        }
        if (!excluded && !(ignored && ignored(nativeDirectory, name))) pending.push_back(entry.path());
      }
    }
  }
}

void SymlinkIndex::setExcludedPaths(const std::vector<std::string> &excludedPaths) {
  std::vector<std::string> keys;
  for (const std::string &path : excludedPaths) {
    keys.push_back(keyOf(path));
  }
  std::lock_guard<std::mutex> lock(mMutex);
  mExcludedKeys.swap(keys);
}

void SymlinkIndex::linkFound(const std::string &directory, const std::string &name) {
  std::lock_guard<std::mutex> lock(mMutex);
  addLink(directory, name);
}

void SymlinkIndex::eventHappened(Change change, bool renamed, const std::string &fromDirectory,
  const std::string &fromName, const std::string &toDirectory, const std::string &toName,
  std::vector<Derived> &derived) {
  std::lock_guard<std::mutex> lock(mMutex);
  // FSEvents can report an entry that appeared as modified, a symlink renamed there among them: it tells a
  // modification by the times of what the symlink leads to.
#if defined(__APPLE__)
  const bool mayBeNew = true;
#else
  const bool mayBeNew = change == Change::CREATED;
#endif
  // Only a new entry can be a symlink not known yet: with none known, nothing else needs a look.
  if (mLinks.empty() && !mayBeNew && !renamed) {
    return;
  }

  const std::string fromEntry = nativeJoin(fromDirectory, fromName);
  const std::string fromKey = pathKey(fromEntry);
  std::set<std::string> moved;
  std::set<std::string> fromExact, fromBelow, toExact, toBelow;
  // A rename that only changes case, where names don't: nothing leads elsewhere.
  bool sameEntry = false;

  // Windows can report a rename that only changes case as the new name created before the old one removed, which
  // has the same key as what is there then: nothing went away.
  if (change == Change::DELETED && !renamed && !mCaseSensitive && isNamedOtherwise(fromEntry)) return;

  const std::string toEntry = renamed ? nativeJoin(toDirectory, toName) : std::string();
  const std::string toKey = renamed ? pathKey(toEntry) : std::string();

  if (renamed) {
    sameEntry = toKey == fromKey;
    // Taken first, since the keys can be the same.
    const std::vector<Link> movedLinks = takeLinksAtOrBelow(fromKey);
    takeLinksAtOrBelow(toKey);
    for (const Link &link : movedLinks) {
      const std::string path = nativeJoin(link.directory, link.name);
      // What follows the moved path, measured on keys, which can be spelled otherwise than the event's path.
      const size_t suffix = (std::min)(link.key.size() - fromKey.size(), path.size());
      const std::string newPath = toEntry + path.substr(path.size() - suffix);
      const size_t separator = newPath.rfind(SEPARATOR);
      const std::string key = addLink(newPath.substr(0, separator), newPath.substr(separator + 1));
      moved.insert(key);
      // Where it still leads, what was reported still holds.
      auto added = mLinks.find(key);
      if (added != mLinks.end() && added->second.chain.back() == link.chain.back()) added->second.shown = link.shown;
    }
    if (mLinks.count(toKey) == 0) {
      moved.insert(addLink(toDirectory, toName));
    }
    if (!sameEntry) linksThrough(toKey, toExact, toBelow);
  } else if (change == Change::DELETED) {
    takeLinksAtOrBelow(fromKey);
#if defined(__APPLE__)
    // FSEvents tells what is still there by following symlinks: a dangling one can be reported as deleted.
    moved.insert(addLink(fromDirectory, fromName));
#endif
  } else if (mayBeNew) {
    // Nothing is below a new entry yet: what was there went away unseen, while excluded say.
    if (change == Change::CREATED) takeLinksAtOrBelow(fromKey);
    moved.insert(addLink(fromDirectory, fromName));
  }
  if (!sameEntry) linksThrough(fromKey, fromExact, fromBelow);

  if (change == Change::MODIFIED && !renamed) {
    // A directory reported as modified (Windows does whenever an entry in it changes) changes nothing below it.
    fromBelow.clear();
    reconcile(fromKey, fromExact, fromBelow, Change::MODIFIED, false, false, moved, derived);
    return;
  }
  // What a directory created holds is reported after it, by every watcher, but not what is below a symlink to one.
  bool plainDirectory = false;
  if (change == Change::CREATED && !renamed && !fromBelow.empty()) {
    std::error_code error;
    plainDirectory = fs::symlink_status(toPath(fromEntry), error).type() == fs::file_type::directory;
  }
  reconcile(fromKey, fromExact, fromBelow, renamed || change == Change::DELETED ? Change::DELETED : Change::CREATED,
    false, plainDirectory, moved, derived);
  if (renamed) {
    const size_t fromSide = derived.size();
    reconcile(toKey, toExact, toBelow, Change::CREATED, true, false, moved, derived);
    // A symlink that lost what it led to and found it again within one rename (another file renamed over a symlink in
    // its chain) only had what it leads to replaced.
    for (size_t created = fromSide; created < derived.size(); ++created) {
      if (derived[created].change != Change::CREATED) continue;
      for (size_t deleted = 0; deleted < fromSide; ++deleted) {
        if (derived[deleted].change != Change::DELETED || derived[deleted].directory != derived[created].directory ||
          derived[deleted].name != derived[created].name) continue;
        derived[deleted].change = Change::MODIFIED;
        derived[deleted].targetDirectory = derived[created].targetDirectory;
        derived[deleted].targetName = derived[created].targetName;
        derived.erase(derived.begin() + created--);
        break;
      }
    }
  }
}

// Brings the symlinks leading through the path an event was about up to date, and reports what it means for them, by
// what the event says happened there: whatever is on disk by now, its opposite may already be queued. Each one is
// looked up again first, so one gone unseen is dropped instead of reported. Where the event can't tell what it means
// (a symlink in the middle of a chain replaced, a directory above the target moved in), what is on disk tells,
// against what was last reported. renamedTo: the event is the arrival side of a rename, which replaces what was there.
// plainDirectory: the event created a directory, whose contents are reported next.
void SymlinkIndex::reconcile(const std::string &key, const std::set<std::string> &exact,
  const std::set<std::string> &below, Change change, bool renamedTo, bool plainDirectory,
  const std::set<std::string> &skipped, std::vector<Derived> &derived) {
  std::set<std::string> keys(exact);
  keys.insert(below.begin(), below.end());
  for (const std::string &linkKey : keys) {
    if (skipped.count(linkKey) > 0) continue;
    auto found = mLinks.find(linkKey);
    if (found == mLinks.end()) continue;
    Link &link = found->second;
    const std::string oldTarget = link.chain.back();
    const bool wasReported = reported(link);
    bool present = false;
    if (!resolve(link, present)) {
      removeLink(linkKey);
      continue;
    }
    const bool isReported = reported(link);
    // A removal is of what it led to before, a creation of what it leads to now.
    const bool atTarget = exact.count(linkKey) > 0 &&
      (change == Change::DELETED ? oldTarget : link.chain.back()) == key;
    const bool above = exact.count(linkKey) == 0;
    auto show = [&](Change shown) {
      link.shown = shown != Change::DELETED;
      if (shown == Change::DELETED ? wasReported : isReported) emit(shown, link, derived);
    };

    if (change == Change::DELETED) {
      if (link.shown) show(Change::DELETED);
    } else if (atTarget && change == Change::MODIFIED) {
      // FSEvents can report a file that appeared as modified.
      if (link.shown || present) show(link.shown ? Change::MODIFIED : Change::CREATED);
    } else if (atTarget && change == Change::CREATED) {
      if (!link.shown) {
        show(Change::CREATED);
      } else if (renamedTo) {
        // Saved by renaming another file over it.
        show(Change::MODIFIED);
      }
    } else if (above && change == Change::CREATED && plainDirectory) {
      // The creation of the target comes with what the directory holds.
    } else if (link.shown && !present) {
      show(Change::DELETED);
    } else if (!link.shown && present) {
      show(Change::CREATED);
    } else if (link.shown && link.chain.back() != oldTarget) {
      show(Change::MODIFIED);
    }
  }
}

std::string SymlinkIndex::keyOf(const std::string &path) const {
  return comparable(withoutTrailingSeparators(toGeneric(path)), mCaseSensitive);
}

// A path as a watcher reports it, through its real directory when it is below a symlink to a directory in the tree,
// which the watcher may report that directory's paths through: symlinks lead to real paths, and the path stays the
// same whichever way the watcher reports it. Looked up only below such a symlink.
std::string SymlinkIndex::physical(const std::string &path) const {
  if (!mThroughDirectoryLinks || mTreeDirectoryLinks.empty()) return path;
  const std::string key = keyOf(path);
  bool aliased = false;
  for (size_t slash = key.find('/', 1); slash != std::string::npos && !aliased; slash = key.find('/', slash + 1)) {
    aliased = mTreeDirectoryLinks.count(key.substr(0, slash)) > 0;
  }
  return aliased ? fromGeneric(spelled(realParent(toPath(path)))) : path;
}

std::string SymlinkIndex::pathKey(const std::string &path) const {
  return keyOf(physical(path));
}

// A real path as the watchers spell it: below the watched path, through the watched path as it was given.
std::string SymlinkIndex::spelled(const fs::path &real) const {
  const std::string generic = toGeneric(fromPath(real));
  const std::string key = comparable(generic, mCaseSensitive);
  if (key == mRealWatchedKey || isBelowKey(key, mRealWatchedKey)) {
    std::string rest = generic.substr(generic.size() - (key.size() - mRealWatchedKey.size()));
    if (!rest.empty() && rest[0] == '/') rest.erase(0, 1);
    return rest.empty() ? mWatchedPath : join(mWatchedPath, rest);
  }
  return generic;
}

std::string SymlinkIndex::nativeJoin(const std::string &directory, const std::string &name) const {
  if (directory.empty() || directory.back() == SEPARATOR) return directory + name;
  return directory + SEPARATOR + name;
}

bool SymlinkIndex::isBelowKey(const std::string &key, const std::string &ancestor) const {
  if (ancestor.empty()) return false;
  if (key.size() <= ancestor.size() || key.compare(0, ancestor.size(), ancestor) != 0) return false;
  return ancestor.back() == '/' || key[ancestor.size()] == '/';
}

bool SymlinkIndex::isExcludedKey(const std::string &key) const {
  for (const std::string &excluded : mExcludedKeys) {
    if (key == excluded || isBelowKey(key, excluded)) return true;
  }
  return false;
}

bool SymlinkIndex::reported(const Link &link) const {
  return link.followed && !isExcludedKey(keyOf(nativeJoin(link.directory, link.name))) &&
    !isExcludedKey(link.chain.back());
}

// Returns its key, empty if it isn't a symlink.
std::string SymlinkIndex::addLink(const std::string &directory, const std::string &name) {
  const std::string path = physical(nativeJoin(directory, name));
  const size_t separator = path.rfind(SEPARATOR);
  const std::string key = keyOf(path);
  auto found = mLinks.find(key);
  if (found != mLinks.end()) {
    // Found again: it may lead elsewhere now. Where it still leads, it is as its events left it, which a lookup now
    // can't tell apart from a change whose event is still queued.
    Link &link = found->second;
    const std::string target = link.chain.back();
    link.directory = path.substr(0, separator);
    link.name = path.substr(separator + 1);
    bool present = false;
    if (!resolve(link, present)) {
      removeLink(key);
      return std::string();
    }
    if (link.chain.back() != target) link.shown = present;
    return key;
  }
  Link link;
  link.key = key;
  link.directory = path.substr(0, separator);
  link.name = path.substr(separator + 1);
  bool present = false;
  if (!resolve(link, present)) return std::string();
  link.shown = present;
  mLinks.emplace(key, std::move(link));
  return key;
}

void SymlinkIndex::removeLink(const std::string &key) {
  auto found = mLinks.find(key);
  if (found == mLinks.end()) return;
  unindex(found->second);
  mLinks.erase(found);
}

// Follows link again from what is on disk now. False if it isn't a symlink anymore. present: what it leads to exists.
bool SymlinkIndex::resolve(Link &link, bool &present) {
  const std::string path = nativeJoin(link.directory, link.name);
  unindex(link);
  link.chain.clear();
  link.followed = false;
  present = false;
  link.treeDirectory = false;
  link.targetDirectory.clear();
  link.targetName.clear();
  link.through.clear();

  fs::path current = toPath(path);
  bool ended = false;
  bool isDirectory = false;
  std::string target;
  for (int hop = 0; hop <= MAX_HOPS && !ended; ++hop) {
    std::error_code error;
    const fs::file_status status = fs::symlink_status(current, error);
    const bool isLink = !error && fs::is_symlink(status);
    if (hop > 0) {
      target = spelled(current);
      link.chain.push_back(comparable(target, mCaseSensitive));
    } else if (!isLink) {
      return false;
    }
    if (!isLink) {
      present = !error && fs::exists(status);
      // Following it: a junction isn't a symlink, but leads to a directory too.
      isDirectory = present && fs::is_directory(current, error);
      ended = true;
      break;
    }
    const fs::path next = fs::read_symlink(current, error);
    if (error) break;
    const fs::path written = next.is_absolute() ? next : current.parent_path() / next;
    current = realParent(written);
    dependOnLinksIn(written, link);
  }
  if (link.chain.empty()) {
    // It couldn't be read: as good as gone.
    return false;
  }

  if (ended && isDirectory) {
    link.treeDirectory = isBelowKey(link.chain.back(), mWatchedKey);
  } else if (ended) {
    const std::string targetKey = link.chain.back();
    if (isBelowKey(targetKey, mWatchedKey)) {
      const size_t slash = target.rfind('/');
      link.targetDirectory = fromGeneric(target.substr(0, slash));
      link.targetName = target.substr(slash + 1);
      link.followed = true;
    }
  }
  index(link);
  return true;
}

// The real path a symlink leads to hides the symlinks to directories written in it, which retargeting or removing
// changes where it leads too. Below the path as written, which events through it are spelled like; and each symlink in
// it by its own real path, for one written through another.
void SymlinkIndex::dependOnLinksIn(const fs::path &written, Link &link) const {
  const std::string lexical = comparable(spelled(written.lexically_normal()), mCaseSensitive);
  if (isBelowKey(lexical, mWatchedKey)) link.through.push_back(lexical);
  // Upwards from the directory it is written in, by parent_path(), which handles every spelling of a root.
  for (fs::path prefix = written.parent_path(); !prefix.empty(); prefix = prefix.parent_path()) {
    const fs::path name = prefix.filename();
    if (name.empty() || prefix == prefix.parent_path()) break;
    if (name == "." || name == "..") continue;
    // Only in the tree, where events can come from.
    if (!isBelowKey(comparable(spelled(prefix), mCaseSensitive), mWatchedKey)) break;
    // Looked up at its real path: through another symlink to a directory, Windows doesn't always tell it is one.
    const fs::path real = realParent(prefix);
    std::error_code error;
    if (!fs::is_symlink(fs::symlink_status(real, error))) continue;
    const std::string key = comparable(spelled(real), mCaseSensitive);
    if (isBelowKey(key, mWatchedKey) && key != lexical) link.through.push_back(key);
  }
}

void SymlinkIndex::index(const Link &link) {
  if (link.treeDirectory) mTreeDirectoryLinks.insert(link.key);
  for (const std::string &path : link.chain) {
    mThrough[path].insert(link.key);
  }
  for (const std::string &path : link.through) {
    mThrough[path].insert(link.key);
  }
}

void SymlinkIndex::unindex(const Link &link) {
  if (link.treeDirectory) mTreeDirectoryLinks.erase(link.key);
  auto drop = [this, &link](const std::string &path) {
    auto found = mThrough.find(path);
    if (found == mThrough.end()) return;
    found->second.erase(link.key);
    if (found->second.empty()) mThrough.erase(found);
  };
  for (const std::string &path : link.chain) drop(path);
  for (const std::string &path : link.through) drop(path);
}

void SymlinkIndex::linksThrough(const std::string &key, std::set<std::string> &exact,
  std::set<std::string> &below) const {
  auto found = mThrough.find(key);
  if (found != mThrough.end()) exact.insert(found->second.begin(), found->second.end());
  const std::string prefix = key.back() == '/' ? key : key + "/";
  for (auto i = mThrough.lower_bound(prefix); i != mThrough.end() && i->first.compare(0, prefix.size(), prefix) == 0;
    ++i) {
    below.insert(i->second.begin(), i->second.end());
  }
}

// Removes the symlink at key and the ones below it, and returns them.
std::vector<SymlinkIndex::Link> SymlinkIndex::takeLinksAtOrBelow(const std::string &key) {
  std::vector<std::string> keys;
  if (mLinks.count(key) > 0) keys.push_back(key);
  const std::string prefix = key.back() == '/' ? key : key + "/";
  for (auto i = mLinks.lower_bound(prefix); i != mLinks.end() && i->first.compare(0, prefix.size(), prefix) == 0;
    ++i) {
    keys.push_back(i->first);
  }
  std::vector<Link> links;
  for (const std::string &linkKey : keys) {
    auto found = mLinks.find(linkKey);
    unindex(found->second);
    links.push_back(std::move(found->second));
    mLinks.erase(found);
  }
  return links;
}

void SymlinkIndex::emit(Change change, const Link &link, std::vector<Derived> &derived) const {
  derived.push_back({ change, link.directory, link.name, link.targetDirectory, link.targetName });
}
