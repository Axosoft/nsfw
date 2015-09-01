var FileWatcher = require('./build/Release/FileWatcher.node');
var path = require('path');
var fs = require('fs');

var nsfw = module.exports = function(watchPath, pollCallback, errorCallback, pInterval) {
  if (!errorCallback || !(errorCallback instanceof Function)) {
    throw new Error('Must provide an error callback');
  }

  this.errorCallback = errorCallback;
  this.poll;
  watchPath = path.resolve(watchPath);

  try {
    fs.statSync(watchPath);
  } catch(error) {
    throw error;
  }

  this.interval = pInterval || 1000;
  this.watcher = new FileWatcher.NSFW(watchPath, pollCallback);
}

// methods
nsfw.prototype.start = function() {
  try {
    this.watcher.start();
    var that = this;
    this.poll = setInterval(function() {
      try {
        that.watcher.poll();
      } catch(error) {
        clearInterval(that.poll);
        this.errorCallback(error);
      }
    }, this.interval);
    return true;
  } catch(error) {
    this.errorCallback(error);
    return false;
  }
};

nsfw.prototype.stop = function(callback) {
  try {
    clearInterval(this.poll);
    this.watcher.stop(callback);
    return true;
  } catch(error) {
    this.errorCallback(error);
    return false;
  }
};
