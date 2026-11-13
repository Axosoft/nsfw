# API

```javascript
const nsfw = require('nsfw');
```

## [*fn*] nsfw(watchPath: string, eventCallback: fn, options: object) : Promise
 - **watchPath**: the path that nsfw should watchPath
 - **eventCallback**: callback that will be fired when NSFW has change events
 - **options**
   - **debounceMS**: time in milliseconds to debounce the event callback
   - **errorCallback**: callback to fire in the case of errors
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
  separators), even if a later call has already replaced them.

## Excluded paths
  - Every excluded path must be absolute; a relative one makes the promises of **nsfw()** and
    **updateExcludedPaths()** reject.
  - Paths outside the watched path are ignored: they are left out of the list the watcher uses and of what
    **getExcludedPaths()** and **updateExcludedPaths()** resolve with. The check is case-insensitive on Windows only,
    and there the `\\?\` form of a path counts as the same path.
  - The watched path itself is accepted, but what it does differs: on macOS and Windows it drops every event, on Linux
    it drops none.
  - Trailing separators are dropped: `/home/me/project/build/` is stored as `/home/me/project/build`.
  - Paths match on whole path components: excluding `/home/me/project/build` excludes the events inside it, but not
    the ones in `/home/me/project/build-tools`.
  - When watching a single file the excluded paths are only stored and returned; they cannot exclude anything.

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
