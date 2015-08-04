var objectAssign = require('object-assign');
var EventEmitter = require('events').EventEmitter;
var nsfw = require('../build/Release/sentinel.node');
var _ = require('lodash');


var sentinel = module.exports = function(paths, time) {
  this.paths = normalizePaths(paths);
  this.time == time || 60;
  this.watcher = new nsfw(this.paths, this.emit, );
});

sentinel.prototype.addWatch = function addWatch(paths) {
  paths = normalizePaths(paths);
  var toAdd = [];
  paths.forEach(function(path) {
    if (this.paths.indexOf(path) == -1) {
      this.paths.push(path);
      if (toAdd.indexOf(path) == -1) {
        toAdd.push(path);
      }
    }
  }, this);
  this.watcher.addWatch(toAdd);
};

sentinel.prototype.removeWatch = function addWatch(path) {
  paths = normalizePaths(paths);
  var toRemove = [];
  this.paths = this.paths.filter(function(path) {
    var keep = paths.indexOf(path) == -1;
    if (!keep) {
      toRemove.push(keep);
    }
    return keep;
  });
  this.watcher.removeWatch(toRemove);
};

assign(sentinel, EventEmitter.prototype);

function normalizePaths(paths) {
  if (!paths) {
    return [process.CWD()];
  }
  if (_.isArray(paths)) {
    return paths;
  }
  else if (_.isString(paths)) {
    return [paths];
  }
  else {
    throw new Error('invalid argument, must be empty, a string, or an array of strings');
  }
}
