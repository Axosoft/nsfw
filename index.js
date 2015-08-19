var FileWatcher = require('./build/Release/FileWatcher.node');
var path = require("path");

var nsfw = module.exports = function(watchPath, callback, pInterval) {
  this.poll;
  watchPath = path.resolve(watchPath);
  this.interval = pInterval || 1000;
  this.watcher = new FileWatcher.NSFW(watchPath, callback);
}

// methods
nsfw.prototype.start = function() {
  this.watcher.start();
  var that = this;
  this.poll = setInterval(function() {
    try {
      that.watcher.poll();
    } catch(error) {
      clearInterval(that.poll);
      throw error;
    }
  }, this.interval);
};

nsfw.prototype.stop = function(callback) {
  clearInterval(this.poll);
  this.watcher.stop(callback);
};
