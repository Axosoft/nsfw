var FileWatcher = require('./build/Release/FileWatcher.node');

var nsfw = module.exports = function(path, callback, pInterval) {
  var poll;
  var interval = pInterval || 1000;
  var watcher = new FileWatcher.NSFW(path, callback);

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
