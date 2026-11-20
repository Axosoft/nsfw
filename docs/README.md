# API

```javascript
const nsfw = require('nsfw');
```

## [*fn*] nsfw(watchPath: string, eventCallback: fn, options: object) : Promise
 - **watchPath**: the path that nsfw should watchPath
 - **eventCallback**: callback that will be fired when NSFW has change events
 - **options**
   - **debounceMS**: time in milliseconds to debounce the event callback
   - **errorCallback**: callback to fire in the case of errors, including an overflow of the inotify event queue on
     Linux, which means events were lost
   - **excludedPaths**: array of absolute paths inside **watchPath** whose events are not reported; the ones outside
     it are ignored, see [excluded paths](#excluded-paths)


  Returns a Promise that resolves to the created NSFW Object.

### eventCallback(events: Array)
  - **events**: An array of **ChangeEvents**

## [*class*] NSFW

### start() : Promise
  Returns a Promise that resolves when the NSFW object has started watching the path.

### stop() : Promise
  Returns a Promise that resolves when NSFW object has halted.

### getExcludedPaths() : Promise
  Returns a Promise that resolves with the current excluded paths.

### updateExcludedPaths(excludedPaths: Array) : Promise
  Replaces the excluded paths, also while the watcher is running. Returns a Promise that rejects if an excluded path
  is not valid, and otherwise resolves with the paths this call set (without the ignored ones and trailing
  separators), even if a later call has already replaced them. Calls made after it, awaited or not, reach the
  watcher after the update, so they wait while it reads the filesystem.

## Excluded paths
  - Every excluded path must be absolute; a relative one makes the promises of **nsfw()** and
    **updateExcludedPaths()** reject.
  - Paths outside the watched path are ignored and omitted from the returned lists. Containment uses whole path
    components, case-insensitively on Windows and according to the volume on macOS; Linux is case-sensitive.
  - A path context is created once by **nsfw()** and reused by **updateExcludedPaths()** and later starts. On Linux and
    macOS the watched path is resolved with filesystem realpath, preserving the physical target of `symlink/..`.
    Repointing a watched symlink afterwards does not change that target. Exclusions can use the original alias or
    the physical root, also after the original symlink is repointed. Windows uses absolute namespaced (`\\?\`) paths internally; plain
    and namespaced input forms are equivalent. Windows junction/short-name aliases are not resolved.
    For a watch path containing `symlink/..`, append a suffix to the original string or use the physical root:
    `path.join(watchPath, 'child')` can collapse it to a different directory, which is outside and ignored.
  - On Linux and macOS any other spelling is read from disk until it reaches the watched root, and `..` is always
    resolved by the filesystem, so after a symlink it is the parent of the symlink's target: watching
    `/home/me/project`, `/home/me/link/../project/build` and `/home/me/project/link/..` are outside and ignored
    when `link` points into another directory, and so is `/home/me/project/link/missing/../build`, since `..` reads
    the names before it. The complete prefix through the last `..` must exist and be traversable.
    Nonexistent components are no longer cancelled: `/home/me/missing/../project/build` is ignored.
    Plain future children without `..` remain supported. Each prefix must succeed as one cumulative lookup,
    so a path through a dangling symlink, a symlink loop or more symlinks than the system
    follows in one lookup, a file followed by `..` or a directory that cannot be searched, also inside a symlink
    target, is ignored. Windows resolves `..` lexically, like the system does, and does not treat a name with
    trailing dots or spaces as an alias of the name without them.
  - Returned exclusions use the canonical root, with `.`, `..`, repeated separators and trailing separators removed
    (roots retain their separator). For example, `/tmp/project/build` may be returned as `/private/tmp/project/build`
    on macOS. Windows returns namespaced paths.
  - Excluded children need not exist. Below the watched root, names are lexical: a symlink named in that suffix is
    not resolved unless `..` follows it. Excluding `/home/me/project/build` excludes events inside it, but not inside `build-tools`. Directory
    creation/deletion events for an excluded directory itself can still be reported by directory watchers.
  - On Linux, a directory that two paths inside the watched path lead to (through a symlink, say) is reported at the
    one scanned first, and an excluded path through either excludes what is below it: with a symlink `a-link` to
    `z-real`, excluding `z-real/build` also excludes `a-link/build`. `z-real` is reported at its own path once
    `a-link` no longer leads to it or stops being watched. Not covered:
    - Excluding `z-real` itself does not stop its events under `a-link`.
    - An excluded path does not apply through an excluded directory, nor through a symlink that leads outside the
      watched path (an absolute one has to give the watched path with its symlinks resolved).
    - A symlink that gets on an excluded path later (created, repointed or moved there) does not exclude what is
      already watched until the next **updateExcludedPaths()**, or until that directory or one above it is renamed.
    - An excluded path written through `a-link` keeps applying once `a-link` is removed, until then as well.
  - Excluding the watched root suppresses every event detected from then on, on every platform. Removing it with
    **updateExcludedPaths()** resumes reporting. This also applies when watching a single file, including its deletion and recreation;
    previously single-file exclusions were only stored and returned.

### Reported paths
  Directory watchers retain the caller's event path spelling on Linux and Windows. Windows resolves ordinary paths
  and uses backslashes, retaining a trailing separator for direct-child events; nested directories have one separator
  between components. Explicit namespaced inputs retain their caller spelling. Linux retains its original spelling:
  one final slash is removed for direct-child events and nested directories append a slash, including repeated ones.
  Both **directory** and **newDirectory** use this
  spelling. macOS directory watchers continue reporting physical realpaths. Single-file pollers retain the caller's
  directory and filename spelling on every platform.

## [*struct*] ChangeEvent
  - **action**: the type of event that occurred
  - **directory**: the location the event took place
  - **file**: the name of the file that was changed _(Not available for rename events)_
  - **oldFile**: the name of the file before a rename _(Only available for rename events)_
  - **newDirectory**: the new location of the file _(Only available for rename events, only useful on linux)_
  - **newFile**: the name of the file after a rename _(Only available for rename events)_


## [*enum*] actions: object
  Is found on the nsfw module as a property. Used to parse event callback.
  - **CREATED**
  - **DELETED**
  - **MODIFIED**
  - **RENAMED**
