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

## Usage

```js
var nsfw = require('nsfw');
return nsfw(
  'dir1',
  function(events) {
    // handle events
  })
  .then(function(watcher) {
    return watcher.start();
  })
  .then(function() {
    // we are now watching dir1 for events!
  });

// With options
return nsfw(
  'dir2',
  function(events) {
  // handles other events
  },
  {
    debounceMS: 250,
    errorCallback(errors) {
      //handle errors
    }
  })
  .then(function(watcher) {
    return watcher.start();
  })
  .then(function() {
    // we are now watching dir1 for events!
  });
```

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
