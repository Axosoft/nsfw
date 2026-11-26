# node-sentinel-file-watcher

[![Tests](https://github.com/Axosoft/nsfw/actions/workflows/tests.yaml/badge.svg?branch=main)](https://github.com/Axosoft/nsfw/actions/workflows/tests.yaml?query=branch%3Amain)
[![npm](https://img.shields.io/npm/v/nsfw)](https://www.npmjs.com/package/nsfw)

A simple file watcher library for node.

## Why NSFW?
NSFW is a native abstraction for Linux, Windows, and macOS file watching services which tries to keep a consistent interface and feature set across operating systems. NSFW offers recursive file watching into deep file systems all at no additional cost to the Javascript layer. In Linux, NSFW recursively builds an inotify watch tree natively, which collects events concurrently to the javascript thread. On macOS, NSFW utilizes the FSEventsService, which recursively watches for file system changes in a specified directory. In Windows, NSFW implements a server around the ReadDirectoryChangesW method.

When NSFW has events and is not being throttled, it will group those events in the order that they occurred and report them to the Javascript layer in a single callback. This is an improvement over services that utilize Node FS.watch, which uses a callback for every file event that is triggered. Every callback FS.watch makes to the event queue is a big bonus to NSFW's performance when watching large file system operations, because NSFW will only make 1 callback with many events within a specified throttle period.

So why NSFW? Because it has a consistent and minimal footprint in the Javascript layer, manages recursive watching for you, and is super easy to use.

## Usage

```js
var nsfw = require('nsfw');
var path = require('path');

var watcher1;
return nsfw(
  '/home/nsfw/dir1',
  function(events) {
    // handle events
  })
  .then(function(watcher) {
    watcher1 = watcher;
    return watcher.start();
  })
  .then(function() {
    // we are now watching dir1 for events!

    // To stop watching
    watcher1.stop()
  });

// With options
var dir2 = '/home/nsfw/dir2';
var watcher2;
return nsfw(
  dir2,
  function(events) {
  // handles other events
  },
  {
    debounceMS: 250,
    errorCallback(errors) {
      //handle errors
    },
    excludedPatterns: ['/node_modules', '*.log', 'tmp/'],
    respectGitignore: true
  })
  .then(function(watcher) {
    watcher2 = watcher;
    return watcher.start();
  })
  .then(function() {
    // we are now watching dir2 for events!

    // we can replace the excluded patterns
    return watcher2.updateExcludedPatterns(['/node_modules', '/.git', '*.log', 'tmp/']);
  })
  .then(function() {
    // To stop watching
    watcher2.stop();
  })
```

## Options

- debounceMS: delays notifications emitted by the library. Default 500 ms. It is the only spacing between two
deliveries of events: since 3.1.0 there is no implicit 50 ms tick, so set it to 50 to keep at least 50 ms between them.
Only events that fill half of the room `maxQueuedEvents` leaves before it ends are delivered at once.
- errorCallback(errors): the library will call this callback when an error happens. For a watched directory,
on Linux that includes an overflow of the inotify event queue (`fs.inotify.max_queued_events`), which means events
were lost: restart the watcher and rescan what you track.
It also includes the watched directory being renamed or deleted, a directory above it included
(`Service shutdown: root path changed (renamed or deleted)`): from then on its path leads elsewhere. On Linux, a rename
of a directory above it is noticed before the watcher handles any further batch of events, which it then drops, before
it delivers any further events, or within about 1 s while the watcher is idle. On Windows, a rename of the watched
directory is noticed at once, or within about 1 s where the directory that holds it can't be watched. Events already
being delivered when the rename happens may still reach `eventCallback` under the old path, before the error.
On Windows an overflow of the system watcher's buffer, which also means events were lost, reads `Buffer filled up and
service needs a restart`, and another unexpected failure of the system watcher reads
`Service shutdown unexpectedly: <system message> (Windows error <code>)`.
On macOS, FSEvents dropping events reads `FSEvents dropped events, events were lost`, and FSEvents coalescing the
changes of a directory tree into a request to scan it again reads
`FSEvents coalesced the events of a directory tree, events were lost`, unless that directory is excluded or ignored.
FSEvents can tell of such a loss in the changes made just before the watcher started, so a watcher started during
heavy activity, such as a package install, may report it at once.
On every platform, more events than `maxQueuedEvents` waiting to be delivered to JavaScript, which also means events
were lost, reads `Event queue exceeded maxQueuedEvents, events were lost`.
A directory watcher retires the failed run and releases that run's native resources before calling `errorCallback`.
Errors queued by a run that has already been explicitly stopped or replaced are discarded. So are the events it had
not delivered yet, including batches already waiting for the JavaScript thread; the same goes for a pause.
After fixing the cause (for example, recreating a deleted directory), call `start()` to watch again;
calling `stop()` first is unnecessary. A previously queued `start()` may already have restarted the watcher,
in which case another `start()` rejects as already running. `stop()` on an already stopped watcher rejects.
For directory watchers, `start()`, `stop()`, `pause()` and `resume()` run in call order.
Thus `stop(); start()` restarts the watcher even when `stop()` is not awaited.
Each operation still returns a promise that should be handled for errors.
A watched file only reports the errors it can't recover from, see [Watching a single file](#watching-a-single-file).
- excludedPaths (deprecated: use `excludedPatterns`, see below): array with the absolute paths we want to exclude
watching. Using it, `updateExcludedPaths` or `getExcludedPaths` emits a `DeprecationWarning` (code
`NSFW_EXCLUDED_PATHS`) once per process; they keep working until a future major release. A relative one makes the
promise `nsfw()` returns reject; the ones outside the watched path are ignored. Paths match on whole path components: excluding
`/home/nsfw/dir2/build` does not exclude `/home/nsfw/dir2/build-tools`. Trailing separators are dropped.
You can update the excludedPaths array without restarting the service using the `updateExcludedPaths` function, which
handles the paths the same way and resolves with the paths that call set, without the ignored ones.
`getExcludedPaths` resolves with the current ones.
To migrate, write each path relative to the watched directory with a leading `/`, which anchors it there:
`path.join(dir, 'node_modules')` becomes `'/node_modules'` and `path.join(dir, 'a', 'b')` becomes `'/a/b'`, with `/`
as separator on every platform. Escape with `\` the characters patterns treat specially (`*`, `?`, `[`, `\`, a leading
`!` or `#`, trailing spaces). Unlike `excludedPaths`, patterns don't apply to a watched file.
- excludedPatterns: array of glob patterns, with the syntax of `.gitignore` files, relative to the watched directory.
What they match is not reported: `*.log` matches at any depth, `/dist` only at the top, `build/` only directories,
`**` spans directories (`src/**/*.gen.ts`) and `!` includes again what an earlier pattern excluded (`!keep.log`).
Nothing inside an excluded directory can be included again. Default `[]`. You can replace them without restarting the
service using the `updateExcludedPatterns` function, which resolves with the patterns that call set;
`getExcludedPatterns` resolves with the current ones.
- respectGitignore: when `true`, what git ignores in the repository the watched directory is in is not reported:
its `.gitignore` files (including the ones above the watched directory), its `info/exclude` and the user's
`core.excludesFile` (`~/.config/git/ignore` by default). Changes to the `.gitignore` files and to `info/exclude` below
the watched directory apply while watching, as long as their events are watched (not when `.git` is itself excluded,
for `info/exclude`); the other files are read when the watcher starts. As in git, a `.gitignore` that is a symlink or
larger than 100 MB is skipped, and the ignore files never exclude a `.git` directory or anything in it. The watched
directory is never excluded either. Outside a repository it has no effect. `excludedPatterns` take precedence over the
ignore files. Set when the watcher is created. Default `false`. Not supported: `include` directives and the
`GIT_CONFIG_*` environment variables (only `/etc/gitconfig`, not on Windows, `$XDG_CONFIG_HOME/git/config`,
`~/.gitconfig` and the repository's config are read for `core.excludesFile`), and nested repositories, which the outer
repository's rules apply to as well. Unlike git, a symlink to a directory counts as a directory for patterns that only
match directories, since the watcher follows it.
Case is ignored for ASCII letters only on case-insensitive filesystems. As git's `safe.directory` check, a repository
another user owns (under `sudo`, another than the one who ran it) is ignored unless `safe.directory` in the system or
user config trusts it; on Windows the owner is the user's SID, or the Administrators group they are a member of, as
git checks it, and paths to another machine read from the repository's files are refused. Nor is a repository looked
for on another filesystem than the watched path's, except on Windows. Only `/etc/gitconfig` (not on Windows),
`$XDG_CONFIG_HOME/git/config` and `~/.gitconfig` are read for `safe.directory`.
- followFileSymlinks: when `true`, macOS and Windows also follow the symlinks to files that were already in the
watched directory when the watcher started, see [Symlinked files](#symlinked-files): `start()` walks the tree once,
in the background. Default `false`: there only the symlinks that appear while watching are followed. No effect on
Linux, which follows every one anyway. Only for a watched directory.

All three filtering options can be combined: a path is excluded when `excludedPaths` excludes it, or when `excludedPatterns` or
the ignore files do, the patterns deciding first, negations included (`!keep.log` reports `keep.log` whatever the
ignore files say). On Linux, the directories excluded by them are not watched at all, which spares inotify watches
(`fs.inotify.max_user_watches`), and neither is a directory a symlink leads to that they exclude. On Linux the watched
directories follow the ignore files: what they now ignore is dropped once they stay unchanged for 200 ms (at most 1 s
after they first changed), and what they no longer ignore is watched once they stay unchanged for 1 s (at most 5 s
after). What either the previous or the new rules ignore never gets a watch meanwhile, not even a directory created or
moved then, so a save or a checkout that rewrites one never has everything it ignores watched for a moment; what the
new rules ignore keeps the watches it had until they are dropped.
Meanwhile events are already filtered by the new rules, and changes in a directory they stop ignoring can be missed. An ignore
file that could never be read (no file descriptor left, say) excludes every directory below it on Linux until it
can; one read before keeps the rules it had. A directory that becomes included again, because an ignore
file or the patterns changed, is watched again without reporting what it already holds, like a path
`updateExcludedPaths` includes again. Events are filtered by the rules in force when they happen: once a path is
excluded, its later changes, its removal included, are not reported even if its creation was. A rename from an included path to an excluded one is reported as a removal, and
one from an excluded path to an included one as a creation. When only patterns for directories (`build/`) match a
path, its removal is judged by what it was when last seen, as its creation was, else by what the system tells (on
Windows nothing, so as a directory: the removal of a file named `build` that was there before watching is not
reported). `excludedPatterns` and `respectGitignore` only apply to
watched directories, not to a watched file.
- nativeBinding: the addon to use instead of the one nsfw finds next to itself, see
[Custom binary location](#custom-binary-location): the absolute path to its `.node` file, or the module a `require()`
of that file returned.
- maxQueuedEvents: the most events that can wait for the JavaScript thread, counting both the ones the watcher has
queued and the ones in batches `eventCallback` hasn't returned from yet. Once the queue holds half of the room those
batches leave, it is delivered without waiting out `debounceMS`, so events only pile up when the thread doesn't keep
up with a burst of changes, a checkout or a package install say, because it is busy or the batches are that large.
One more overflows the queue: the watcher drops them, stops and calls `errorCallback` with
`Event queue exceeded maxQueuedEvents, events were lost`, like an overflow of the system's own queue: restart the
watcher and rescan what you track. A limit below what one native read can queue (a directory tree moved in on
Linux, a full buffer on Windows) can overflow even while the thread is idle. Each event takes a few hundred bytes
natively. Default `1000000`; a positive integer. Only for a watched directory: a watched file keeps the default, and
recovers from an overflow on its own, see [Watching a single file](#watching-a-single-file).

## Watching a single file

`nsfw()` also takes the path of a file. It isn't polled: the directory it is in is watched by the system, natively
filtered down to the file's own entry, so its siblings and what the directory holds cost nothing in JavaScript and, on
Linux, the watcher takes a single inotify watch (two while a directory takes the file's name). As for a watched
directory, the Linux watcher still checks the directory once a second while idle. Events about the file are reported
under the directory and name of the path given, a symlink's rather than its target's:
- `CREATED` when it appears, by a rename onto it too, and `DELETED` when it goes, by a rename away too.
- `MODIFIED` when it changes, its attributes included. A file renamed over it, as editors save, is reported as
  `MODIFIED`, and so is a file removed and created again within the same batch of events (a removal and a creation
  told apart, by a pause after the removal say, are reported as such).

A symlink is followed once, when the watcher is created: the file it led to then is the one watched. Writes through a
hard link in another directory are not reported. `start()`, `stop()`, `pause()` and `resume()` never reject over the
watcher's state: starting or resuming a running watcher, and stopping or pausing one that isn't, do nothing, and
`resume()` starts a stopped watcher. On Windows the directory is open while it is watched, which can keep the
directories above it from being renamed or removed. `debounceMS` above 60000 is taken as 60000.

Unlike a watched directory, a watched file outlives its directory, as the poller of nsfw 3.0.x did:
- When the file's directory is removed or renamed, the file's removal is reported (unless paused), as a single
  `stat()` of the file after the failure finds it gone, and the watcher keeps going without any error. It looks for
  the directory once a second, on whole seconds, with a `stat()` of the directory (not of the file, and on Linux
  without any inotify watch), and watches it again once it is back, reporting `CREATED` if the file is there.
  `pause()`, `resume()` and `stop()` work meanwhile, and `start()` takes a watcher whose directory is missing. These
  looks are taken outside the queue of `start()`, `stop()`, `pause()` and `resume()` calls, so a share that stopped
  answering doesn't hold them. When a pause called after a failure while watching comes before the look at the file,
  a removal of the file is reported on resume, while a file created or changed meanwhile is taken as it is.
- Any other failure of the watcher of the directory, such as its event queue overflowing or, on macOS, FSEvents losing
  events under heavy activity in the directory, doesn't stop it either: the directory is watched again from the next
  whole second on, and a `stat()` of the file then reports what changed meanwhile, `DELETED`, `CREATED`, or
  `MODIFIED` when its size, times, device or inode differ from the last look the watcher took, when it started or last
  recovered. As events may have been lost, a
  change already reported may be reported again as `MODIFIED`.
- `errorCallback` only gets an error when the directory is there but can't be watched again, as when no inotify watch
  is left: once until the directory is watched again, while the watcher keeps trying. Without an `errorCallback`,
  nothing is thrown. `start()` rejects when the directory is there but can't be watched.

## Callback Argument

An array of events as they have happened in a directory, it's children, or to a file.
```js
[
  {
    "action": 2, // nsfw.actions.MODIFIED
    "kind": 0, // nsfw.kinds.FILE
    "directory": "/home/nsfw/watchDir",
    "file": "file1.ext"
  },
  {
    "action": 0, // nsfw.actions.CREATED
    "kind": 1, // nsfw.kinds.DIRECTORY
    "directory": "/home/nsfw/watchDir",
    "file": "folder"
  },
  {
    "action": 1, // nsfw.actions.DELETED
    "kind": 0, // nsfw.kinds.FILE
    "directory": "home/nsfw/watchDir/testFolder",
    "file": "test.ext"
  },
  {
    "action": 3, // nsfw.actions.RENAMED
    "kind": 0, // nsfw.kinds.FILE
    "directory": "home/nsfw/watchDir",
    "oldFile": "oldname.ext",
    "newDirectory": "home/nsfw/watchDir/otherDirectory"
    "newFile": "newname.ext"
  }
]
```

A directory that appears in the watched one is reported as `CREATED`, and so are its visible contents, whether it was
created, copied or moved in. A move between directories inside the watched one is reported as `RENAMED` on Linux and
macOS. On Windows that move is reported as `DELETED` and `CREATED`, followed by `CREATED` events for the directory's
visible contents.

Event are enumerated by the nsfw.actions enumeration
```js
nsfw.actions = {
  CREATED: 0,
  DELETED: 1,
  MODIFIED: 2,
  RENAMED: 3
};
```

What the entry of an event is, by the nsfw.kinds enumeration (for `RENAMED`, the entry that moved)
```js
nsfw.kinds = {
  FILE: 0,
  DIRECTORY: 1,
  UNKNOWN: 2
};
```
`kind` describes the entry itself, not what a symlink leads to: on Linux and macOS a symlink is `FILE`, whatever it
leads to (it is removed as a file, though the watcher follows one to a directory); on Windows a symlink or a junction
to a directory is `DIRECTORY`, as Windows tells it. `FILE` stands for anything that isn't a directory: a regular file,
a symlink, a FIFO, a socket or a device. An event with an empty `file` is about the directory it names itself: on
Linux, a directory a symlink leads to is reported at the symlink, so its own changes come as `FILE`. A watched file
reports `FILE`. Windows looks the entry up when it reads the event, so an entry replaced meanwhile by one of the other
kind (a file removed and a directory created at its path, say) is reported with the kind of the new one.

It is `UNKNOWN` when the system can't tell:
- On Windows, for a removal: the system doesn't tell what a removed entry was. Nor for any other event of an entry
  that is already gone when its event is read (a file modified and removed right after, say).
- On macOS, when FSEvents merged the events of entries of both kinds at the same path (a file replaced by a
  directory, say): for their removal, and for any other event once the entry is gone.
- On Linux, for an entry reported by the scan of a directory that appeared, when it is gone before the scan could
  look it up and the filesystem didn't list its type.
- On every platform, with `excludedPatterns` or `respectGitignore`, for the removal reported when an entry of another
  kind takes the path of one they didn't ignore and they ignore the new one.

## Symlinked files
A symlink below the watched path that leads to a file below it too is reported as that file is: a change to the file
is reported under the file's path and then again under the symlink's path, as a separate event with the same action.
Writing through the symlink changes the file, so it is reported under both paths as well.

- `MODIFIED` when the file is modified, or replaced by another file renamed over it, as editors save (Windows
  reports that as the file removed and then created, and so is the symlink), or when a symlink in the middle of a
  chain is replaced at once by one leading to another file (removing it and creating it again is reported as both).
- `DELETED` when the file is removed or renamed away: the symlink now dangles.
- `CREATED` when the file appears again, created or renamed to the path the symlink leads to.

A chain of symlinks is followed as a whole: each symlink in it is reported, and removing or retargeting a symlink in
the middle is reported under the symlinks that led through it. A cycle is not followed. Neither is a symlink to a
directory (on Linux the watcher follows those into the directory they lead to), a symlink to a path outside the
watched path (even one the Linux watcher reaches through a symlink to a directory outside it), an excluded symlink, or
a symlink that leads to an excluded or ignored file. Symlinks created, removed, renamed or retargeted while watching
are followed from then on. An event reported under a symlink has the `kind` of the symlink itself: `FILE`. A watched
file follows none: it only reports its own changes, under the path it was given.

Which symlinks are followed, and what it costs, depends on the platform:

- Linux: every one, whatever `followFileSymlinks` says. The watcher lists every directory it watches anyway, so it
  finds the symlinks that were there when it started without listing anything more, and without any extra inotify
  watch: it looks up where each symlink leads. The watcher may report the changes in a directory through a symlink to
  it, so for an event below a known symlink to a directory in the tree, the real path of its directory is looked up
  too.
- macOS and Windows: the watcher never lists the tree, so by default only the symlinks created, renamed or retargeted
  while it watches are followed. With `followFileSymlinks: true`, `start()` also walks the tree once, in the
  background, to find the symlinks already there. The walk lists every directory that isn't excluded or ignored (by
  `excludedPaths`, `excludedPatterns` or `respectGitignore`), without going through symlinks or junctions, so it
  costs as much as the tree is large: exclude what you don't need, such as `node_modules/`. On Windows it skips the
  directories a cloud provider keeps online only, which listing would have it fetch. `start()` doesn't wait for it,
  so until it reaches a symlink, a change to that symlink's target is only reported under the target. `stop()` ends
  it.

Whatever the platform, what is kept grows with the number of symlinks only. To learn about new symlinks, each
created or renamed entry is looked up once (on macOS each modified or deleted one too, since FSEvents can report a
new symlink that way); with no symlinks known, nothing else is looked up.

## Linux: inotify limits
On Linux, nsfw watches each directory of the tree with an inotify watch. The kernel limits the number of watches
per user (`fs.inotify.max_user_watches`, shared by every process of the user), and once the limit is reached the
watcher stops and `errorCallback` gets `Inotify limit reached`. nsfw doesn't fall back to polling the directories that
don't fit: polling would have to keep a snapshot of every entry in memory and read the whole tree again and again,
which costs far more memory, CPU and disk access than the watches it replaces. Instead:

- Exclude what you don't need to watch, such as `node_modules/`, build output or `.git`, with `excludedPatterns` or
  `respectGitignore`. Excluded directories don't take a watch.
- Raise the limit. Check it with `sysctl fs.inotify.max_user_watches`, raise it until the next reboot with
  `sudo sysctl fs.inotify.max_user_watches=524288`, and keep it with a file in `/etc/sysctl.d/`, for example
  `echo fs.inotify.max_user_watches=524288 | sudo tee /etc/sysctl.d/90-inotify.conf`. Each watch takes about 1 KB of
  kernel memory on 64-bit systems.

After raising the limit or excluding more, call `start()` again.
The number of inotify instances per user (`fs.inotify.max_user_instances`, 128 by default on many distributions)
limits how many watchers can run at once, and the event queue (`fs.inotify.max_queued_events`) how many events can
wait to be read; see `errorCallback` above.

## Installation
```sh
npm install nsfw
```

NSFW is a native module built on [Node-API](https://nodejs.org/api/n-api.html), so one binary works on every Node
release and in Electron. The package ships a prebuilt binary for these platforms, which installs without a compiler
toolchain:

| Platform | Architectures | Minimum version |
| --- | --- | --- |
| Linux, glibc | x64, arm64 | glibc 2.28 (Debian 10, Ubuntu 20.04, RHEL 8) |
| Linux, musl (Alpine) | x64, arm64 | Alpine with the `libstdc++` package, which Node needs too |
| macOS | x64, arm64 | macOS 11 |
| Windows | x64, arm64 | Windows 10 |

Those are the oldest versions Node 22 runs on too. The binary is picked when the module loads, so a package manager
that skips dependency install scripts, like pnpm or npm 12, works as well. Other musl distributions than Alpine need
`LIBC=musl` in the environment to be detected as such.

On any other platform the install script compiles the addon from source with
[node-gyp](https://github.com/nodejs/node-gyp), which needs the toolchain it lists for your operating system and
the install script to be allowed to run. To compile it on a supported platform as well, install with
`npm install nsfw --build-from-source` (or `npm_config_build_from_source=true` in the environment). A binary in
`build/Release` takes precedence over the prebuilt one.

### Custom binary location
nsfw loads its addon the first time `nsfw()` is called, not when it is imported, from `build/Release` or `prebuilds/`
next to itself. Where the binary is somewhere else, as when a bundler moves nsfw's code into a bundle of its own or an
app ships the binary among its resources, point nsfw to it with:

1. the `nativeBinding` option of `nsfw()`: the absolute path to the `.node` file, or the module `require()` returned
   for it;
2. else the `NSFW_NATIVE_BINDING` environment variable, holding the absolute path to the `.node` file, read by each
   `nsfw()` call without the option.

Without either, nsfw looks next to itself as usual, where the `NSFW_PREBUILD` environment variable of
[node-gyp-build](https://github.com/prebuild/node-gyp-build) names a directory to search instead of nsfw's. An Electron
app that does not bundle nsfw needs none of these: Electron loads the binary from inside `app.asar`.

When no addon can be loaded, importing nsfw still works (so does `nsfw.actions`), and the promise `nsfw()` returns
rejects with an error that tells why, naming the file when one was found but could not be loaded. A path that is not
absolute or does not end in `.node` is rejected with a `TypeError`.

A `.node` file is loaded once per thread, and every watcher created with it there shares that addon. Copies of the binary
in other files load as separate addons, whose watchers work side by side.

The prebuilt binaries are in `prebuilds/<platform>-<arch>/`, named `nsfw.node`, except on Linux, where they are
`nsfw.glibc.node` and `nsfw.musl.node`; a binary built from source is `build/Release/nsfw.node`. For instance, an
Electron app whose main process is bundled can have [electron-builder](https://www.electron.build) copy the prebuilt
binaries for the target to the app's resources:

```json
{
  "build": {
    "extraResources": [{ "from": "node_modules/nsfw/prebuilds/${platform}-${arch}", "to": "nsfw" }]
  }
}
```

and pass the one for the running platform:

```js
const fs = require('fs');
const path = require('path');
const { app } = require('electron');
const nsfw = require('nsfw');

function binaryName() {
  if (process.platform !== 'linux') return 'nsfw.node';
  // The test nsfw makes to pick its own binary when the LIBC environment variable is unset.
  return fs.existsSync('/etc/alpine-release') ? 'nsfw.musl.node' : 'nsfw.glibc.node';
}

const nativeBinding = app.isPackaged ? path.join(process.resourcesPath, 'nsfw', binaryName()) : undefined;

async function watch(dir) {
  return nsfw(dir, events => { /* ... */ }, { nativeBinding });
}
```

A bundler that handles `require()` itself may fail to load a path it did not bundle; load the `.node` file the way it
supports, and pass the module that returns as `nativeBinding`.

## Development
The repository uses [pnpm](https://pnpm.io), pinned through the `packageManager` field of `package.json`. Install the
dependencies, which also builds the addon and compiles the TypeScript in `js/src` to `js/dist`, then lint and test
with:

```sh
pnpm install
pnpm test
```

`pnpm exec prebuildify --napi --strip` builds the binary for the current platform into `prebuilds/` (add `--tag-libc`
on Linux), and `NSFW_TEST_PREBUILT=1 pnpm test` runs the tests against it instead of a build of its own.

### Releasing
1. Rename `## Unreleased` in `CHANGELOG.md` to `## X.Y.Z` (or `## X.Y.Z - YYYY-MM-DD`) and set `"version"` in
   `package.json` to `X.Y.Z`, through a pull request.
2. Tag the merge commit and push the tag:

   ```sh
   git tag vX.Y.Z
   git push origin vX.Y.Z
   ```

The [release workflow](.github/workflows/release.yaml) checks that the tag matches `package.json` and that the
changelog has notes for the version, builds and tests the binaries for every platform, publishes the package to npm
with provenance, and creates the GitHub release of the tag with those notes and the package attached. A version with a
prerelease suffix (`X.Y.Z-beta.1`) goes to npm's `next` dist-tag and is marked as a prerelease. One older than npm's
`latest`, a patch of an earlier major, goes to `latest-<major>` and doesn't become the latest release. After publishing, it
installs the version from npm on every platform and watches a directory with it. Running the workflow by hand on a
branch is a dry run, which shows the notes in its summary: run it on `main` before pushing the tag, with
`gh workflow run release.yaml --ref main`.

npm has to trust the workflow first: on npmjs.com, add `Axosoft/nsfw` with the workflow `release.yaml` as a trusted
publisher of nsfw, or else store a granular access token as the `NPM_TOKEN` secret of the repository.
