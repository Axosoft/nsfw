# node-sentinel-file-watcher
<table>
  <thead>
    <tr>
      <th>Linux</th>
      <th>OS X</th>
      <th>Windows</th>
    </tr>
  </thead>
  <tbody>
    <tr>
      <td colspan="2" align="center">
      <a href="https://travis-ci.org/Axosoft/nsfw"><img src="https://travis-ci.org/Axosoft/nsfw.svg?branch=master"></a>
      </td>
      <td align="center">
        <a href="https://ci.appveyor.com/project/implausible/node-simple-file-watcher"><img src="https://ci.appveyor.com/api/projects/status/79ejlq7e60kjmbl6?svg=true"></a>
      </td>
    </tr>
  </tbody>
</table>
A simple file watcher library for node.

## Why NSFW?
NSFW is a native abstraction for Linux, Windows, and OSX file watching services which tries to keep a consistent interface and feature set across operating systems. NSFW offers recursive file watching into deep file systems all at no additional cost to the Javascript layer. In Linux, NSFW recursively builds an inotify watch tree natively, which collects events concurrently to the javascript thread. In OSX, NSFW utilizes the FSEventsService, which recursively watches for file system changes in a specified directory. In Windows, NSFW implements a server around the ReadDirectoryChangesW method.

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
    excludedPaths: [path.join(dir2, 'node_modules')],
    excludedPatterns: ['*.log', 'tmp/'],
    respectGitignore: true
  })
  .then(function(watcher) {
    watcher2 = watcher;
    return watcher.start();
  })
  .then(function() {
    // we are now watching dir2 for events!

    // we can update excludedPaths array
    return watcher2.updateExcludedPaths([path.join(dir2, 'node_modules'), path.join(dir2, '.git')]);
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
A directory watcher retires the failed run and releases that run's native resources before calling `errorCallback`.
Errors queued by a run that has already been explicitly stopped or replaced are discarded.
After fixing the cause (for example, recreating a deleted directory), call `start()` to watch again;
calling `stop()` first is unnecessary. A previously queued `start()` may already have restarted the watcher,
in which case another `start()` rejects as already running. `stop()` on an already stopped watcher rejects.
For directory watchers, `start()`, `stop()`, `pause()` and `resume()` run in call order.
Thus `stop(); start()` restarts the watcher even when `stop()` is not awaited.
Each operation still returns a promise that should be handled for errors.
- excludedPaths: array with the absolute paths we want to exclude watching. A relative one makes the promise `nsfw()`
returns reject; the ones outside the watched path are ignored. Paths match on whole path components: excluding
`/home/nsfw/dir2/build` does not exclude `/home/nsfw/dir2/build-tools`. Trailing separators are dropped.
You can update the excludedPaths array without restarting the service using the `updateExcludedPaths` function, which
handles the paths the same way and resolves with the paths that call set, without the ignored ones.
`getExcludedPaths` resolves with the current ones.
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

## Installation
NSFW is a native node module and requires Node-Gyp to be functional before you can install it.
Make sure you have completed installing all of the dependencies listed for [Node-Gyp](https://github.com/nodejs/node-gyp) on your operating system.
