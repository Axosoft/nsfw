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


  Returns a Promise that resolves to the created NSFW Object.

### eventCallback(events: Array)
  - **events**: An array of **ChangeEvents**

## [*class*] NSFW

### start() : Promise
  Returns a Promise that resolves when the NSFW object has started watching the path.

### stop() : Promise
  Returns a Promise that resolves when NSFW object has halted.

## [*struct*] ChangeEvent
  - **action**: the type of event that occurred
  - **directory**: the location the event took place
  - **file**: the name of the file that was changed _(Not available for rename events)_
  - **oldFile**: the name of the file before a rename _(Only available for rename events)_
  - **newFile**: the name of the file after a rename _(Only available for rename events)_


## [*enum*] actions: object
  Is found on the nsfw module as a property. Used to parse event callback.
  - **CREATED**
  - **DELETED**
  - **MODIFIED**
  - **RENAMED**
