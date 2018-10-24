#!/usr/bin/env node
/* eslint no-console: 0 */

const path = require('path');
const nsfw = require('./lib/src/index.js');

function usage () {
  console.log('Usage: nsfw <pattern> [<pattern>...] [options]');
  console.log('  -h, --help\tShow help');
  console.log('  -v, --verbose\tMake output more verbose');
}

function start (dirs, verbose) {
  const options = {
    errorCallback: function (err) {
      console.error('Error:', err);
    }
  };

  const eventCallback = function (events) {
    events.forEach(function (event) {
      switch (event.action) {
      case nsfw.actions.CREATED:
        console.log('Created:', path.join(event.directory, event.file));
        break;
      case nsfw.actions.DELETED:
        console.log('Deleted:', path.join(event.directory, event.file));
        break;
      case nsfw.actions.MODIFIED:
        if (verbose) {
          console.log('Modified:', path.join(event.directory, event.file));
        }
        break;
      case nsfw.actions.RENAMED:
        console.log('Renamed:', path.join(event.directory, event.oldFile), '→', event.newFile);
        break;
      }
    });
  };

  dirs.forEach(function (dir) {
    nsfw(dir, eventCallback, options).then(function (watcher) {
      if (verbose) {
        console.log('Watching', dir);
      }
      return watcher.start();
    });
  });
}

function main (argv) {
  const dirs = [];
  let verbose = false;

  argv.forEach(function (arg, i) {
    if (i === 0) {
      return;
    }
    if (i === 1 && path.basename(argv[0]) === 'node') {
      return;
    }
    if (arg === '-h' || arg === '--help') {
      return usage();
    } else if (arg === '-v' || arg === '--verbose') {
      verbose = true;
    } else {
      dirs.push(arg);
    }
  });

  if (dirs.length === 0) {
    return usage();
  }

  start(dirs, verbose);
}

main(process.argv);
