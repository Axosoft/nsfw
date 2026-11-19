const fs = require('fs');
const path = require('path');
const util = require('util');

exports.DEBOUNCE = 1000;
exports.TIMEOUT_PER_STEP = 3000;
exports.WORKDIR = path.resolve(__dirname, '../../mockfs');

exports.sleep = util.promisify(setTimeout);

// The inotify watches this process holds (Linux).
exports.countInotifyWatches = function() {
  let count = 0;
  for (const fd of fs.readdirSync('/proc/self/fd')) {
    let info;
    try {
      info = fs.readFileSync(`/proc/self/fdinfo/${fd}`, 'utf8');
    } catch (error) {
      // Closed meanwhile.
      continue;
    }
    count += (info.match(/^inotify wd:/gm) || []).length;
  }
  return count;
};

// Waits for the inotify watches this process holds to reach expected (Linux).
exports.watchCountReaches = async function(expected, tries = 100) {
  for (let i = 0; i < tries && exports.countInotifyWatches() !== expected; ++i) {
    await exports.sleep(50);
  }
  require('assert').strictEqual(exports.countInotifyWatches(), expected);
};
