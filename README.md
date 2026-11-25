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

- debounceMS: delays notifications emitted by the library. Default 500 ms.
- errorCallback(errors): the library will call this callback when an error happens.
On Linux that includes an overflow of the inotify event queue (`fs.inotify.max_queued_events`), which means events
were lost: restart the watcher and rescan what you track.
It also includes the watched directory being renamed or deleted, a directory above it included
(`Service shutdown: root path changed (renamed or deleted)`): from then on its path leads elsewhere. On Linux, a rename
of a directory above it is noticed before the watcher handles any further batch of events, which it then drops, before
it delivers any further events, or within about 1 s while the watcher is idle. On Windows, a rename of the watched
directory is noticed at once, or within about 1 s where the directory that holds it can't be watched. Events already
being delivered when the rename happens may still reach `eventCallback` under the old path, before the error.
A directory watcher retires the failed run and releases that run's native resources before calling `errorCallback`.
Errors queued by a run that has already been explicitly stopped or replaced are discarded.
After fixing the cause (for example, recreating a deleted directory), call `start()` to watch again;
calling `stop()` first is unnecessary. A previously queued `start()` may already have restarted the watcher,
in which case another `start()` rejects as already running. `stop()` on an already stopped watcher rejects.
For directory watchers, `start()`, `stop()`, `pause()` and `resume()` run in call order.
Thus `stop(); start()` restarts the watcher even when `stop()` is not awaited.
Each operation still returns a promise that should be handled for errors.
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

All three options can be combined: a path is excluded when `excludedPaths` excludes it, or when `excludedPatterns` or
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

## Callback Argument

An array of events as they have happened in a directory, it's children, or to a file.
```js
[
  {
    "action": 2, // nsfw.actions.MODIFIED
    "directory": "/home/nsfw/watchDir",
    "file": "file1.ext"
  },
  {
    "action": 0, // nsfw.actions.CREATED
    "directory": "/home/nsfw/watchDir",
    "file": "folder"
  },
  {
    "action": 1, // nsfw.actions.DELETED
    "directory": "home/nsfw/watchDir/testFolder",
    "file": "test.ext"
  },
  {
    "action": 3, // nsfw.actions.RENAMED
    "directory": "home/nsfw/watchDir",
    "oldFile": "oldname.ext",
    "newDirectory": "home/nsfw/watchDir/otherDirectory"
    "newFile": "newname.ext"
  }
]
```

Event are enumerated by the nsfw.actions enumeration
```js
nsfw.actions = {
  CREATED: 0,
  DELETED: 1,
  MODIFIED: 2,
  RENAMED: 3
};
```

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
