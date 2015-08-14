var FileWatcher = require('./build/Release/FileWatcher.node');
var path = require("path");

var nsfw = module.exports = function(watchPath, callback, pInterval) {
  var poll;
  watchPath = path.resolve(watchPath);
  var interval = pInterval || 1000;
  var watcher = new FileWatcher.NSFW(watchPath, callback);

  // methods
  this.start = function() {
    watcher.start();
    poll = setInterval(function() {
      try {
        watcher.poll();
      } catch(error) {
        clearInterval(poll);
        throw error;
      }
    }, interval);
  };

  this.stop = function() {
    clearInterval(poll);
    watcher.stop();
  };

}
