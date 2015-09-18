#!/usr/bin/nodejs
var nsfw = require('./');
var sh = require('segfault-handler');

sh.registerHandler();
var watcher;
function generateWatcher() {
  console.log('creating new watcher');
  return new nsfw(
    '../Github/gitkraken',
    function(events) {
      console.log('ping');
      events.forEach(function(event) {
        console.log(event.file);
      });
    },
    function(errors) {
      console.log('error: ', errors);

      watcher = null;
      watcher = generateWatcher();
      watcher.start();
    }
  );
}
watcher = generateWatcher();

watcher.start();
