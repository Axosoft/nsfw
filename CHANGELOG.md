# Changelog

## 3.2.0

### Added
- `maxQueuedEvents` bounds the events that wait to be delivered to JavaScript, which grew without bound in native
  memory while a burst of changes went on and the JavaScript thread was busy (#281). One more than it stops a
  directory watcher with `Event queue exceeded maxQueuedEvents, events were lost` in `errorCallback`, like an
  inotify queue overflow on Linux or a buffer overflow on Windows. Defaults to 1000000. Events that fill half of the
  room it leaves are delivered without waiting out `debounceMS`, so a burst alone doesn't overflow it while
  JavaScript keeps up.
- A prebuilt binary for FreeBSD x64, built on FreeBSD 14.5 against the inotify of its base system, so installing nsfw
  there needs neither a compiler nor the `libinotify` package (#285). FreeBSD 14.4 and earlier lack that inotify:
  install `libinotify` there and build from source, as before. A build from source uses the base system's inotify
  wherever `/usr/include/sys/inotify.h` is there, and `libinotify` otherwise.

### Changed
- A directory watcher drops the batches of events still waiting for the JavaScript thread when it is stopped, paused
  or fails, as a watched file already did, instead of delivering them afterwards (#281).

### Fixed
- On Linux, a watcher whose root couldn't be watched closed its inotify descriptor twice: once when `start()` failed
  and again when it was released, by then possibly a file or socket another part of the process had opened under the
  same number (#274). A `start()` that failed with an exception no longer leaves the descriptor and its watches
  behind, and the descriptor is no longer inherited by child processes.
- macOS: events FSEvents lost are reported through `errorCallback` and stop a directory watcher, like an inotify queue
  overflow on Linux or a buffer overflow on Windows, instead of being silently missed (#275). Dropped events read
  `FSEvents dropped events, events were lost`. Changes FSEvents coalesced into a request to scan a directory again read
  `FSEvents coalesced the events of a directory tree, events were lost`, unless that directory is excluded or ignored.
  A watched file recovers from such a loss as, since 3.1.1, from any other failure of its watcher, and takes no notice
  of changes coalesced below its directory.
- On Linux, a symlink to a file in a directory the watched tree reaches through a symlink to it is reported under one
  path again: since 3.1.0, the changes of what it leads to were reported under its real path, while its own creation
  and removal came under the path the tree watches the directory through (#276).
- Windows: a watcher on a network share whose server rejects the 1 MiB read buffer at once no longer stops with
  `Service shutdown unexpectedly: The parameter is incorrect. (Windows error 87)`: it opens the directory again and
  retries with a 64 KiB buffer, the largest a share takes, as it already retried when the read failed on completion,
  and frees the larger one (#280). A read that fails so with a 64 KiB buffer is reported instead of being retried
  forever. A watched file's watcher reads into 64 KiB buffers instead of 1 MiB ones, and a watcher allocates its second
  buffer only once it has read events, so a watcher no longer takes 2 MiB from the start. A burst of changes in a
  watched file's directory overflows the smaller buffer sooner, from which the file's watcher recovers on its own.
- A watched file no longer indexes the symlinks in its directory: on Linux every one of them when it started, nor
  resolves again every one leading to the file on each of its changes (#278). Starting next to 5,000 symlinks to the
  file took about 50 ms and 5 MiB, and each change of the file about 45 ms more. A watched file only reports its own
  entry, so its watcher now follows no symlink, on any platform; a watched directory follows them as before.
- The removal of a directory excluded by a pattern for directories (`build/`) is no longer reported when it replaced a
  file of that name while `updateExcludedPatterns()` had removed the pattern, replaced it with others, or excluded a
  directory it is in, and then brought it back (#277).
- Event batches and watcher errors still queued for JavaScript are freed when the Node environment tears down, as when a
  `Worker` that watches a busy tree is terminated, or when the call that queues them fails. They used to leak (#273).

### Performance
- With `respectGitignore`, judging whether a changed path is ignored no longer takes time quadratic in its depth (#279):
  whether each directory is ignored, and which `.gitignore` files apply in it, is remembered until the rules change,
  instead of being worked out again for every ancestor of every path. With a `*.o` rule, an event 32 directories deep
  took about 65 µs and now under 2 µs. Time it with `js/scripts/benchmark-gitignore-depth.cpp`.

## 3.1.1

### Fixed
- A watched file outlives its directory again, as it did up to 3.0.x. Since 3.1.0, when the directory holding the file
  was renamed or removed, the watcher reported the file's removal, then gave `errorCallback` `Service shutdown: root
  path changed (renamed or deleted)` and stopped; without an `errorCallback`, that error was thrown, uncaught, which
  could end the process. Now the file's removal is reported (unless paused, and only if it was there), nothing reaches
  `errorCallback`, and the watcher keeps going: it looks for the directory once a second, on whole seconds, with a
  `stat()` of the directory, not of the file and without any inotify watch on Linux, and watches it again once it is
  back, reporting `CREATED` if the file is there. `pause()`, `resume()` and `stop()` work meanwhile, and `start()`
  takes a watcher whose directory is missing instead of rejecting with `NSFW was unable to start watching that
  directory.` These looks don't hold `start()`, `stop()`, `pause()` or `resume()` on a share that stopped answering,
  and when a pause called after a failure while watching comes before the look at the file, a removal of the file is
  reported on resume, while a file created or changed meanwhile is taken as it is.
- Any other failure of the watcher of a watched file's directory, such as its event queue overflowing under heavy
  activity in the directory or a network share going away, no longer stops it either, nor reaches `errorCallback`: the
  directory is watched again from the next whole second on, and a `stat()` of the file then reports what changed
  meanwhile: `DELETED`, `CREATED`, or `MODIFIED` when its size, times, device or inode differ from the last look the
  watcher took, when it started or last recovered. As events may have been lost, a change already reported may be
  reported again as `MODIFIED`. Only when the directory is there but can't be watched again, as when no inotify watch
  is left, `errorCallback` gets the error, once until the directory is watched again, and the watcher keeps trying;
  without an `errorCallback` nothing is thrown. `start()` still rejects when the directory is there but can't be
  watched. Directory watchers are unchanged.

### Changed
- A note on 3.1.0, which changed it without saying so (#265): the delivery thread no longer wakes up every 50 ms, so
  `debounceMS` is the only spacing between two deliveries of events. With a small `debounceMS`, events can now be
  delivered more often than every 50 ms; to keep at least 50 ms between them, set `debounceMS: 50`.

## 3.1.0

### Added
- `nativeBinding` option and `NSFW_NATIVE_BINDING` environment variable, to load the addon from the absolute path of
  a `.node` file of your choice, such as one an Electron app ships among its resources or one a bundler moved. The
  option also takes an addon that was already loaded, and takes precedence over the variable.
- Every event tells what its entry is in a new `kind` field, from the new `nsfw.kinds` enumeration: `FILE`,
  `DIRECTORY` or `UNKNOWN` when the system can't tell, as on Windows for a removal (#119). For `RENAMED`, it is the
  entry that moved. A symlink is a `FILE` on Linux and macOS, whatever it leads to; on Windows a symlink or a junction
  to a directory is a `DIRECTORY`. Windows looks the entry up for each event but a removal. Linux takes it from the
  event, or for an entry the scan of a new directory reports, from what that scan already looked up. macOS takes it
  from the event's flags or from what the watcher already looked up, and only looks the entry up when the flags don't
  tell it, for a modification or an entry that moved elsewhere in the watched path. A watched file reports `FILE`.
  The `FileChangeEvent` types have the field, so code that builds events of those types has to set it.
- A change to a file is also reported under each symlink below the watched path that leads to it, when the file is
  below the watched path too: modified, deleted (the symlink dangles) and created again as the file is. Chains of
  symlinks are followed, cycles, symlinks to directories, to paths outside the watched path and excluded ones aren't.
  On Linux every such symlink is followed, found by the scan the watcher already does, with no extra inotify watch.
  On macOS and Windows the ones created, renamed or retargeted while watching are, and with the new
  `followFileSymlinks` option also the ones already there when the watcher starts, which costs a walk of the tree in
  the background at `start()`. See "Symlinked files" in the README for what each platform looks up.

### Changed
- The addon is loaded by the first `nsfw()` call instead of when nsfw is imported, so importing it no longer throws
  when no binary can be found: the promise `nsfw()` returns rejects instead.
- A directory watcher no longer wakes up every 50 ms to check for events and errors: its delivery thread sleeps until
  the native watcher queues an event or records an error. The first events after a quiet period are delivered as soon
  as they are queued, instead of up to 50 ms later; `debounceMS` still groups the ones that follow a delivery. An
  error is reported as soon as it is recorded, also while the events that follow a delivery wait out `debounceMS`.
- On Linux, a rename of a directory above the watched path is noticed before each batch of events is handled, before
  each delivery, and within about 1 s when the watcher is idle: its inotify thread checks the watched path once a
  second, on whole seconds so the watchers of a process wake together. It now also notices a directory above being
  replaced by another one that holds the same path. Events already being delivered when the rename happens may still be
  reported under the old path.
- On Windows, a rename of the watched directory is noticed through the directory that holds it, as resolved through
  junctions, symlinks and subst drives, and before each batch of events is handled. Only where that directory can't
  be watched does the watcher check the path once a second.
- Watching a single file no longer polls it (#134). Instead of a `stat()` of the file every `debounceMS`, costly on
  network storage, the directory it is in is watched by the system, natively filtered down to the file's entry: on
  Linux that takes a single inotify watch, whatever the directory holds (two while a directory takes the file's
  name), and on Windows the directory is watched without its subdirectories. The events stay the same, `CREATED`,
  `DELETED` and `MODIFIED` under the directory and name of the path given, including `MODIFIED` for a file renamed
  over the watched one. What differs:
  - When the file's directory is removed or renamed, the file's removal is reported (unless paused), then
    `errorCallback` gets `Service shutdown: root path changed (renamed or deleted)` and the watcher stops, as a
    directory watcher does; without an `errorCallback` that error is thrown. `start()` watches again once the
    directory is back, and can reject with `NSFW was unable to start watching that directory.` until then. Before,
    the removal was reported and the file kept being polled, so its creation was reported once the directory came
    back.
  - Changes are reported as they happen, as for a directory, instead of as one `stat()` per `debounceMS` saw
    them: a removal and a creation that fall in two batches are reported as such, where one `stat()` could see
    them as a single `MODIFIED`. A `debounceMS` above 60000 is taken as 60000.
  - Writes through a hard link in another directory are not reported.
  - On Windows, the watched directory is open while it is watched, which can keep the directories above it from being
    renamed or removed; a watched symlink is followed once, when the watcher is created, as on Linux and macOS; and a
    change of the file's attributes is reported as `MODIFIED`, as a change of its change time was.
  - Any error of the watcher of the directory, such as its event queue overflowing under heavy activity in the
    directory, stops the file watcher and reaches `errorCallback`, thrown without one, as for a directory. The poller
    never failed. On Linux, the directory is checked once a second while idle, as a watched directory is.
  - An `errorCallback` that is neither a function nor falsy makes `nsfw()` reject with `TypeError:
    options.errorCallback must be a function.`, as for a directory. Before, a file watcher ignored it.

### Fixed
- On Windows, a read of the watched directory that was aborted (`ERROR_OPERATION_ABORTED`, as when its pending I/O is
  cancelled) no longer stops the watcher with `Service shutdown unexpectedly`: while the watcher is stopping it is
  ignored, otherwise the read is issued again, and only a third read aborted in a row is reported (#16). The other
  unexpected failures say why: `Service shutdown unexpectedly: <system message> (Windows error <code>)`, for example
  `Service shutdown unexpectedly: The specified network name is no longer available (Windows error 64)`, instead of
  `Service shutdown unexpectedly` alone. A read that completed without writing anything, which means that more
  changed than its buffer holds, is reported as `Buffer filled up and service needs a restart`, which stops the
  watcher as when the system reports the overflow itself, instead of reporting the events of an earlier read again.
- On Windows, a `RENAMED` event takes its `newDirectory` from the new path, not from the old one (#150). Windows reports
  as a rename only an entry that stays in its directory, so both were the same in the events seen so far; a move to
  another directory is still reported as `DELETED` and `CREATED`.
- On Windows, a rename whose old name ends one read of the system watcher and whose new name starts the next is
  reported as one `RENAMED`, as when both come in the same read, instead of `DELETED` for the old name and `CREATED`
  for the new one (#26). The old name waits for the next read up to 10 ms, and is reported as `DELETED` when the next
  read doesn't start with its new name, fails or overflows, or doesn't come in time. An old name followed by anything
  but its new name in the same read no longer makes the watcher skip that next change. A rename from an included path
  to a top-level one `excludedPaths` excludes is reported as `DELETED` instead of `RENAMED`, as for `excludedPatterns`.
- On Windows, a directory that appears in the watched directory (moved in from outside it, moved from another directory
  inside it, which Windows reports as a removal and a creation, or renamed from a path `excludedPaths`,
  `excludedPatterns` or the ignore files exclude) has what it holds reported as `CREATED` too, after the directory
  itself, as on Linux (#73); a directory that `excludedPaths` names deeper than the top level is itself reported, so its
  rename to an included name stays a `RENAMED`, followed by what it holds. Before, only the directory was reported when
  it was moved, so its files went unnoticed. The directories excluded by `excludedPaths`, `excludedPatterns` or the
  ignore files are not looked into, and neither are symlinks and junctions, which the system doesn't watch through, nor
  the directories whose entries a cloud provider hasn't brought locally, which listing them would fetch. A directory
  copied in, whose entries Windows reports one by one, still has each reported once: the entries the scan of the
  directory found are not reported again when their own creation comes in the next two reads of the system watcher;
  a rename to one of them is reported as it comes, as on Linux. A `DELETED` or `RENAMED` for one of them in between, or
  a read that fails, ends that, so that a later creation is reported.
- On macOS, a file created and then changed within the same second is reported as `CREATED` once and then
  `MODIFIED` (#5). FSEvents repeats the flags of a path's earlier changes with each new one, and the watcher told a
  creation from a change by whether the file was born and last modified in the same second. It now remembers the
  entries it reported, while they are there, and otherwise looks at when the entry was born. An entry created shortly
  before the watcher started can still be misreported.
- On macOS, a file created and removed before the watcher handled it is reported as `CREATED` and then `DELETED`, as
  on the other platforms, rather than as `DELETED` alone.
- On macOS, more renames are reported as `RENAMED` (#78): FSEvents adds the flags of earlier changes to either side of
  a rename, such as for a file created or modified shortly before, and only a side with nothing but the renamed flag
  could be paired, so such a rename was reported as `MODIFIED` and `DELETED`, or `CREATED` and `DELETED`. The sides are
  now paired by the inode FSEvents reports for each, or by their consecutive event ids when it doesn't report one, and
  other renames in the same directory no longer break the pairing. Consecutive renames can be combined into a single
  event. When combining them would overwrite another rename's source, the affected names are reported as deleted
  and then created.
- On macOS, a move between two directories of the watched path is reported as `RENAMED`, with `newDirectory` set to
  the destination's, as on Linux (#150). Before, only renames within a directory were paired.
- On macOS, a rename that only changes the case of a name (`index.js` to `Index.js`) on a case-insensitive volume is
  reported as `RENAMED` (#146), instead of as two `MODIFIED`.
- On macOS, an entry moved into the watched path from outside, such as a folder restored from the Trash, is reported
  as `CREATED`, and so is what a directory holds, as on Linux, except what the excluded patterns or the ignore files
  exclude (#31). Before, it could be reported as `MODIFIED`, and what a directory held wasn't reported. An entry moved
  out, such as to the Trash, is reported as `DELETED`, or as `RENAMED` when FSEvents delivers its other side in
  another batch and the entry is still found below the watched path on the same volume.

### Documentation
- The README explains the Linux inotify limits: what happens when `fs.inotify.max_user_watches` is reached, how to
  exclude directories or raise the limit, and why nsfw doesn't fall back to polling.

## 3.0.1

Documentation only: the code and the prebuilt binaries are the same as in 3.0.0.

### Changed
- The README shows the status of the tests and the version on npm, instead of the badges of Travis CI and AppVeyor,
  which the project no longer builds on.

## 3.0.0

nsfw now ships prebuilt binaries for Linux, macOS and Windows on x64 and arm64, so installing it there needs no
compiler toolchain. This is a major release, with breaking changes:
- Node 18 and 20 are no longer supported (`engines.node` is `>=22`).
- The addon is loaded through `node-gyp-build`, so a bundler has to leave nsfw external or copy `prebuilds/` along.
- Excluded paths behave differently: calls that used to succeed can now reject, and paths that used to be forwarded
  to the watcher are now ignored.
- On Linux, a watcher whose inotify queue overflows now stops with an error instead of losing events silently.

### Added
- Prebuilt binaries for Linux x64 and arm64 (glibc 2.28 or later, and musl), macOS x64 and arm64 (macOS 11 or later)
  and Windows x64 and arm64, so installing nsfw on them needs no compiler toolchain, nor its install script to run.
  On other platforms the install script still compiles the addon from source, and `--build-from-source` compiles it
  on these too.
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
- On macOS the addon is compiled as `gnu++17` for macOS 11.0, whatever the Node or Electron headers it is built
  against, instead of C++11 and macOS 10.7. Linux and Windows keep using the standard of the headers, C++17 or newer.
- The addon is loaded through `node-gyp-build`, which picks `build/Release/nsfw.node` when there is one and the
  prebuilt binary for the platform in `prebuilds/` otherwise, instead of requiring `build/Release/nsfw.node`. A bundler
  has to leave nsfw external, or copy `prebuilds/` along. The `install` script is `node-gyp-build`, which only compiles
  when no prebuilt binary loads.
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

## 2.3.1

### Security (development only)
Yarn resolutions, which only apply to development installs: the code published is the same as 2.3.0's.
- Force `flatted` ≥ 3.4.2 to fix prototype pollution (GHSA-rf6f-7fwh-wjgh)
- Force `picomatch` ≥ 2.3.2 to fix method injection (GHSA-3v7f-55p6-f55p)
- Force `serialize-javascript` ≥ 7.0.5 to fix CPU exhaustion (GHSA-qj8w-gfj5-8c6v)

## 2.3.0

### Security
- Update `minimatch` 3.1.2 → 3.1.3 and 5.1.6 → 5.1.8 to fix ReDoS vulnerability
- Force `serialize-javascript` ≥ 7.0.3 via yarn resolution to fix RCE vulnerability (CVE-2020-7660)

### Breaking (development only)
- Node.js 20+ is now required for development/testing due to `serialize-javascript` v7. This does **not** affect consumers of the package at runtime.
- CI no longer tests against Node.js 18
