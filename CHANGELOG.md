# Changelog

## Unreleased

This needs a major release: Node 18 and 20 are no longer supported (`engines.node` is `>=22`). Behaviour changes in
excluded paths too: calls that used to succeed can now reject, and paths that used to be forwarded to the watcher are
now ignored. On Linux, a watcher whose inotify queue overflows now stops with an error instead of losing events
silently.

### Added
- `excludedPatterns` option: glob patterns with `.gitignore` syntax, relative to the watched directory, whose matches
  are not reported. `updateExcludedPatterns()` replaces them while watching and `getExcludedPatterns()` returns them.
- `respectGitignore` option: excludes what git ignores in the repository the watched directory is in (its
  `.gitignore` files, followed while watching, its `info/exclude` and `core.excludesFile`).
- On Linux, directories excluded by either option are not watched, sparing inotify watches.

### Deprecated
- `excludedPaths`, `updateExcludedPaths()` and `getExcludedPaths()`, in favour of `excludedPatterns`,
  `updateExcludedPatterns()` and `getExcludedPatterns()`: `/build` excludes `build` in the watched directory. They keep
  working, and emit a `DeprecationWarning` (code `NSFW_EXCLUDED_PATHS`) once per process. They will be removed in a
  future major release.

### Changed
- `nsfw()` and `updateExcludedPaths()` handle excluded paths the same way:
  - Relative paths reject with `Excluded path must be an absolute path.` Before, `updateExcludedPaths()` accepted
    them, and so did `nsfw()` on Windows.
  - Absolute paths outside the watched path are ignored instead of throwing or being forwarded to the watcher, and are
    left out of what `getExcludedPaths()` and `updateExcludedPaths()` resolve with. Before, `nsfw()` rejected them on
    Linux and macOS unless they started with the watched path as a string, like a sibling sharing it as a prefix
    (`/a/foo-bar` when watching `/a/foo`) or `/a/foo/../bar`, while `nsfw()` on Windows and `updateExcludedPaths()`
    forwarded them all. A forwarded ancestor of the watched path excluded every event on macOS and Windows; it is
    now ignored too.
  - Linux and macOS resolve the watched root with filesystem realpath once at construction, preserving physical
    `symlink/..` semantics. Exclusions under that root or an alias pointing there share one canonical form. Windows
    uses absolute namespaced paths. Returned exclusions use that canonical root; future children are supported and
    names below the root remain lexical. macOS containment follows the volume's case sensitivity. `..` in an
    exclusion is resolved by the filesystem, so `symlink/..` keeps its physical meaning: watching `/a/w`,
    `/a/link/../w/x` and `/a/w/link/..` are ignored when `link` points into another directory. The complete prefix through the last `..` must exist and be traversable; missing components are no longer
    cancelled by `..`. Plain future children without `..` remain supported. A path the filesystem cannot look up as spelled, for example through a
    directory that cannot be searched or through more symlinks than one lookup follows, is ignored. A watched path that the filesystem cannot look up, or whose target
    changes while it is resolved, makes `nsfw()` reject.
  - Native watchers receive the same canonical paths used by JS containment. Repeated separators, dot segments,
    trailing separators and symlinked watch roots no longer cause mismatched exclusions. Event directories retain
    caller spelling on Linux and Windows; macOS continues reporting realpaths. A watched symlink is bound to its
    original target across updates and restarts.
  - Excluding the watched root suppresses every event consistently, including on Linux and for single-file pollers.
    Removing that exclusion resumes reporting. Previously Linux ignored root exclusions and file pollers only stored
    them. Directory events for excluded descendants themselves may still be reported.
- Non-array `excludedPaths`, or non-string elements, reject with a `TypeError`; `updateExcludedPaths()` now rejects
  instead of throwing synchronously.
- `updateExcludedPaths()` resolves with the paths that call set, without the ignored ones and rewritten as above;
  before it resolved with whatever the latest call had set.
- Calls made while an `updateExcludedPaths()` is still resolving its paths on disk reach the watcher after it, in call
  order, instead of running with the previous excluded paths.
- All trailing separators of excluded paths are dropped (`\` too on Windows), not just one `/`; roots are kept.
- Typings: code that silenced the old `[string]` type of `excludedPaths` with `// @ts-expect-error` now fails with
  "Unused '@ts-expect-error' directive"; drop the directive.
- Breaking: `engines.node` is `>=22`, the Node releases tested, instead of `>=10.16.0`; Node 18 and 20 have reached
  end of life. `node-addon-api` is bounded to `^8.9.2` instead of `*`.
- On macOS the addon is compiled as `gnu++17` with the deployment target of the Node or Electron headers it is built
  against (macOS 11.0 for Node 22, 13.5 for Node 24 and 26) instead of C++11 and macOS 10.7. Linux and Windows keep
  using the standard of the headers, C++17 or newer.
- On Linux, an inotify event queue overflow fires `errorCallback` with `Inotify event queue overflowed, events were
  lost` and stops the watcher, like any other error. Before, the events past `fs.inotify.max_queued_events` (16384 by
  default) were dropped silently and the watcher kept running, possibly without watching the directories created in
  the meantime. Call `start()` again and rescan what you track, since events were lost. When another error stopped the
  watcher first (for example `Inotify limit reached` during the same burst), `errorCallback` gets that one instead. And
  if the watched directory is gone by the time the error is reported, it gets the root path error, whatever came first.
  It can also be deleted right after an overflow is reported (say, by the `rm -rf` that overflowed the queue), so if
  `start()` then rejects, check that the directory still exists.
- The JavaScript layer is written in TypeScript and published compiled to `js/dist`, with its type declarations
  generated from the source instead of the hand-written `index.d.ts`. The public types are unchanged. Code that
  required `nsfw/js/src/index.js` or referenced `nsfw/index.d.ts` directly has to require `nsfw` instead. Installing
  from git runs the `prepare` script to compile it, so a package manager that blocks dependency scripts has to allow
  nsfw's, as it already had to for building the addon.

### Performance
- Linux: `start()` on a large tree takes about half the time, and the watcher holds about 40% less memory. Measure it
  with `node --expose-gc js/scripts/benchmark-start.js <path>`. The scan now trusts the type a directory listing gives
  each entry, which changes two cases: an entry listed as a file that a symlink to a directory replaces before the scan
  gets to it isn't watched through it (as one created right after the scan never was), and neither is a directory that
  the filesystem lists as something else, which only a faulty FUSE daemon or a corrupted directory entry does.
- Linux: each watched directory takes 16 bytes less memory (about 5% of what the watcher holds), except one moved back
  in, which takes a little more until its rescan.

### Fixed
- A fresh `npm install nsfw` compiles again (#212). `node-addon-api` 8.9.2 headers need C++17, but the build forced
  C++11 on macOS.
- Excluding a directory no longer excludes siblings whose name starts with it (`/a/foo` excluded `/a/foobar`) on
  macOS and Windows.
- `updateExcludedPaths()` no longer races with the watcher threads that read the excluded paths on macOS, Windows and
  Linux, where it also changed the inotify watch tree while events were being handled.
- macOS no longer leaks the excluded path strings on every update and every event.
- On macOS, excluded paths work when the watched path goes through a symlink, such as `/tmp` or `os.tmpdir()`.
  FSEvents reports events under the real path, which the excluded paths, given under the watched path, never matched.
- On Linux, `updateExcludedPaths()` works on directories created after `start()`. It looked their parent up instead,
  so it kept watching the excluded directory and could stop watching an unrelated one with the same name.
- On macOS, excluding a watched `/` also excludes the events of its direct children.
- `getExcludedPaths()` and `updateExcludedPaths()` work when watching a single file instead of throwing a `TypeError`.
- Typings: `excludedPaths` is `string[]`, `getExcludedPaths()` is declared and `updateExcludedPaths()` resolves with
  `string[]`.
- On Linux, a failed read of the inotify descriptor no longer parses the failure as events past the end of the read
  buffer, which could crash the process. It fires `errorCallback` with `Inotify read failed: <reason>` and stops the
  watcher, and an interrupted read is retried. Before, a read that returned no data stopped the watcher with the
  generic `Service shutdown unexpectedly`.
- On Linux, the watcher error and the watched root are no longer read and written from two threads without
  synchronization, which could report a garbled error or crash right when the watcher failed.
- On Linux, an excluded directory created, recreated or moved into place after `start()` is no longer watched, and
  neither is an excluded path below a directory that moves there (#210). Before, their contents were reported, while
  macOS and Windows dropped them. The other way around, a directory that was only excluded under its old path is
  watched once it moves, and what it holds is reported as `CREATED`.
- On Linux, an excluded path also applies when the watcher has its directory at another path, such as a symlink
  scanned before it (#222): with `a-link` a symlink to `z-real`, excluding `z-real/hidden` stops the events reported
  under `a-link/hidden`. Before, only `updateExcludedPaths()` did, for a directory watched at that moment.
- On Linux, a directory watched under a symlink stays watched, at its own path from then on, once the symlink no
  longer leads to it or stops being watched (#223). Before, the directory was no longer watched until the watcher
  was restarted, or its events kept the path of the symlink.
  This also holds when the symlink leads through another one that is removed, repointed or moved, such as
  `a-link -> b-link` with `b-link -> real` (#228), also by a path that leaves the watched directory and comes back.
  Before, the directory's events kept the path of the outer symlink. A symlink on the way inside an excluded
  directory is only looked at again once an update includes that directory.
- On Linux, moving a directory into the one a symlink inside it leads to, such as `a/x -> ../z-real` and then
  `mv a z-real/moved`, no longer crashes the watcher (#230). The `RENAMED` event reports the destination's own path,
  `z-real/moved`, instead of one through the symlink, and the directories it held at symlinks stay watched at their
  own paths, also when one is below another, such as `a/b -> ../zz/z-real` with `a/c -> ../zz`. When no symlink on
  the way has a place of its own to take, as when it leads out of the watched directory, the move is reported as
  `DELETED`, since the directory is no longer watched.
- On Linux, a directory moved out of the watched path, or into an excluded directory, is no longer watched. Before, its
  events kept being reported under its old path, and it wasn't watched again when moved back in under another path.
  Moved back, whether its old watches were removed already or not, it is watched like a directory that moves in for the
  first time: it and what it holds then are reported as `CREATED`. A change in it between the move back and the moment
  its directory is watched again has no event of its own, so a `DELETED`, `RENAMED` or `MODIFIED` for it can be
  missing, as for a directory that moves in for the first time. Before, a deletion right after a move back to the same
  path was reported, since its watches were never removed. The event read
  right after such a move is no longer also reported as `CREATED`, and a move into or out of such a directory right
  after it left is reported as a creation or a deletion. A move split across two reads of the inotify descriptor is
  reported as one `RENAMED` instead of `DELETED` and `CREATED`; for that, a move out of the watched path is reported
  once no other event comes for 10 ms, or right before the next one.
- On Linux, a directory moved into one that was just created is watched at its new path, also when a symlink takes its
  old place, and so is one that a rename moved while it was being scanned, or created in a directory renamed before its
  creation was handled. A directory that leaves for a place that can't be watched, while a symlink or a bind mount
  created meanwhile still leads to it from the watched path, is watched there, with the exclusions of that path.
- On Linux, `updateExcludedPaths()` removes the inotify watches of the directories it excludes a few at a time, while
  few events are waiting to be read, instead of all at once. Each removal queues an event, so excluding a tree of more
  directories than `fs.inotify.max_queued_events` (16384 by default) overflowed the inotify queue, and events were lost.
  The events of the excluded directories stop being reported right away. One that keeps having events has its watch
  removed at its next one. The directories that stop being watched because they moved away or onto an excluded path have
  their watches removed the same way, but not at their next event. When the limit of watches is reached, such watches
  are removed to make room while the queue is less than about half full (estimated from its size in bytes); a directory
  that can't be watched meanwhile is tried again after a read once any pending rename is handled, while such watches
  wait. Once watched, its contents are reported as `CREATED`, including when `updateExcludedPaths()` brought the
  directory back into scope. The error stays terminal once there are no dropped watches left. A directory found again
  while its old watch isn't removed yet (moved back, included again by `updateExcludedPaths()`, seen by a scan before
  its move back is handled, or found below one of those) keeps that
  watch, but none of the events it queued before it was found are reported, also once it is back at a visible path,
  since some may be from while it couldn't be watched. The directories found together share a marker, an event queued
  right after the last of them was found: their events before it are dropped, the later ones reported. All the paths
  one `updateExcludedPaths()` call or one move includes again are found together, so whatever their number they take
  one marker; what happens in such a directory while the call is still running may not be reported. Each is scanned
  after that marker is queued. A scan triggered by a move back reports visible entries as `CREATED`. Re-including a
  path normally resumes watching silently, except for a deferred retry at the watch limit. Recovery scans required by
  a concurrent rename can report existing descendant contents again to avoid losing newly created entries.
  Directories found again by
  those scans get a marker of their own, queued after them, one per level of the tree. As for a directory that moves in
  for the first time, changes between its move back and its level's marker can lack their own `DELETED`, `RENAMED`
  or `MODIFIED` event. A file created and deleted again in that interval goes unreported. Marker generation shares the
  retirement budget between reads; scans that exhaust that budget wait for the reader to make room. An explicit
  re-inclusion waits for its paced scans before resolving; recovery from concurrent renames can continue afterward.
  If `stop()` is requested meanwhile, the update can yield to shutdown and resolves with its own requested list once
  that run has ended.
- On Linux, a directory that was not watched (excluded, for instance) renamed over an empty watched directory is
  watched.
- On Linux, a file created in a directory right after the directory itself is reported as `CREATED` once (#229).
  Before, when it was created between the directory being watched and being scanned, both the scan and its own event
  reported it.

## 2.3.0

### Security
- Update `minimatch` 3.1.2 → 3.1.3 and 5.1.6 → 5.1.8 to fix ReDoS vulnerability
- Force `serialize-javascript` ≥ 7.0.3 via yarn resolution to fix RCE vulnerability (CVE-2020-7660)

### Breaking (development only)
- Node.js 20+ is now required for development/testing due to `serialize-javascript` v7. This does **not** affect consumers of the package at runtime.
- CI no longer tests against Node.js 18
