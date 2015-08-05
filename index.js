var objectAssign = require('object-assign');
var EventEmitter = require('events').EventEmitter;
var nsfw = require('../build/Release/sentinel.node');
var _ = require('lodash');


var sentinel = module.exports = function(path, time) {
  this.path = path;
  this.time == time || 60;
  this.watcher = new nsfw(this.path, this.emit);
});

assign(sentinel, EventEmitter.prototype);
