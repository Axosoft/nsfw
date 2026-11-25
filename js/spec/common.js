const assert = require('assert');
const fs = require('fs');
const path = require('path');
const test = require('node:test');
const util = require('util');

exports.DEBOUNCE = 100;
exports.WORKDIR = path.resolve(__dirname, '../../mockfs');

exports.sleep = util.promisify(setTimeout);

// node:test's it() with a timeout of its own, as mocha's this.timeout() gave: --test-timeout would bound every suite
// and every file's process as a whole too.
exports.it = (name, options, fn) => typeof options === 'function' ?
  test.it(name, { timeout: 120000 }, options) : test.it(name, { timeout: 120000, ...options }, fn);
exports.it.skip = test.it.skip;

// What read() returns for each descriptor this process holds (Linux), skipping those closed meanwhile.
exports.readDescriptors = function(read) {
  const results = [];
  for (const fd of fs.readdirSync('/proc/self/fd')) {
    try {
      results.push(read(Number(fd)));
    } catch (error) {
      // Closed meanwhile.
      if (error.code !== 'ENOENT') throw error;
    }
  }
  return results;
};

// The inotify watches this process holds (Linux).
exports.countInotifyWatches = () => exports.readDescriptors(fd => fs.readFileSync(`/proc/self/fdinfo/${fd}`, 'utf8'))
  .reduce((count, info) => count + (info.match(/^inotify wd:/gm) || []).length, 0);

// Polls predicate() until it holds or timeout runs out, and returns whether it held.
exports.eventually = async function(predicate, { timeout, interval = 25 }) {
  const deadline = Date.now() + timeout;
  while (!(await predicate())) {
    if (Date.now() >= deadline) {
      return false;
    }
    await exports.sleep(interval);
  }
  return true;
};

// Waits until predicate() holds. timeout is a generous upper bound for slow runners, not a delay: a predicate that
// already holds returns at once. message (a string or a function, so it can describe the state at the deadline) is
// the assertion's.
exports.waitUntil = async function(predicate, { timeout = 15000, interval = 25, message = 'Never held' } = {}) {
  if (!(await exports.eventually(predicate, { timeout, interval }))) {
    assert.fail(typeof message === 'function' ? message() : message);
  }
};

// Waits for one of the events recorded so far to match predicate.
exports.waitForEvent = (events, predicate, description) => exports.waitUntil(() => events.some(predicate),
  { message: () => `No ${description} in ${JSON.stringify(events)}` });

// Creates a new file in directory and waits for its creation: events are delivered in order, so once it is
// reported, so is everything that happened before it.
let barriers = 0;
exports.barrier = async function(events, directory) {
  const file = `barrier-${process.pid}-${barriers++}.file`;
  await fs.promises.writeFile(path.join(directory, file), 'Barrier');
  const { CREATED } = require('../dist').actions;
  await exports.waitForEvent(events, event => event.action === CREATED && event.directory === directory &&
    event.file === file, `creation of ${file}`);
};

// A later write lands in a different timestamp second, even on filesystems with whole-second precision.
exports.untilNextSecond = async function(filePath) {
  const { birthtimeMs, mtimeMs } = await fs.promises.stat(filePath);
  const nextSecondMS = (Math.floor(Math.max(birthtimeMs, mtimeMs) / 1000) + 1) * 1000;
  await exports.sleep(Math.max(0, nextSecondMS - Date.now() + 50));
};

// Waits for the inotify watches this process holds to reach expected (Linux).
exports.watchCountReaches = async function(expected, tries = 100) {
  await exports.waitUntil(() => exports.countInotifyWatches() === expected,
    { timeout: tries * 50, interval: 50,
      message: () => `${exports.countInotifyWatches()} inotify watches, not ${expected}` });
};

exports.mkdirp = directory => fs.promises.mkdir(directory, { recursive: true });

exports.remove = target => fs.promises.rm(target, { recursive: true, force: true });

exports.outputFile = async (file, data) => {
  await exports.mkdirp(path.dirname(file));
  await fs.promises.writeFile(file, data);
};
