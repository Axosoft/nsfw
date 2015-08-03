# node-simple-file-watcher
a simple node wrapper for [simplefilewatcher](https://code.google.com/p/simplefilewatcher/)

```js
var simple = require('node-simple-file-watcher');
var watcherOne = new simple(); // watches CWD
var watcherTwo = new simple('dir1'); // watches CWD/dir1
var watcherThree = new simple(['dir2', '/dir3']); // watches CWD/dir2 and /dir3

watcherTwo.on('add', function doAddHandling(filePath) {
  console.log('Added:', filePath);
});
```

A watcher is a simple event emitter watching a single directory or file. 
There are 3 events fired: 'add', 'delete', and 'modify'. 
