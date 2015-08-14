# node-sentinel-file-watcher
A simple file watcher library for node.

```js
var simple = require('node-sentinel-file-watcher');
var watcherOne = new nsfw("dir1", function(events) {
  // handle events
});
var watcherTwo = new nsfw("dir2", function(events) {
  // handles other events
}, 5000); // every 5 seconds

```
