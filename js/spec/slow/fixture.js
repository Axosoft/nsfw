const { after, afterEach, before, beforeEach } = require('node:test');
const assert = require('assert');
const cp = require('child_process');
const fs = require('fs');
const fsp = require('fs/promises');
const path = require('path');
const nsfw = require('../../dist');
const { WORKDIR: workDir, mkdirp, outputFile, remove, sleep } = require('../common');

module.exports = function fixture() {
  const watchedPath = path.join(workDir, 'watched');
  const outsidePath = path.join(workDir, 'outside');
  const markerSignal = watchedPath + '.nsfw-test-one-marker';
  const touch = file => fs.closeSync(fs.openSync(path.join(watchedPath, file), 'w'));

  async function watchWhile(test, excludedPaths = []) {
    const events = [];
    const watch = await nsfw(watchedPath, batch => events.push(...batch), { debounceMS: 100, excludedPaths });
    try {
      await watch.start();
      await test(events, watch);
    } finally {
      touch('nsfw-test-release-event-loop');
      await watch.stop();
    }
  }

  async function waitFor(events, predicate, description) {
    for (let i = 0; i < 100 && !events.some(predicate); ++i) {
      await sleep(100);
    }
    assert.ok(events.some(predicate), `No ${description} in ${JSON.stringify(events)}`);
  }

  // Checks, once the events are handled, that what was reported about name in directory adds up to whether it
  // exists, and that directory is watched. One that is gone needs no creation left standing; and if it was at that
  // path before, where the consumer could know of it, a deletion of its own or of an ancestor after its last creation
  // too. One that was only ever there out of sight (say, it came from an excluded path) needs no deletion.
  async function checkReturnState(events, directory, name, exists, knownBefore = false) {
    await barrier(events);
    const file = path.join(directory, name);
    assert.strictEqual(await fsp.access(file).then(() => true, () => false), exists);
    const actions = events.filter(event => event.directory === directory && event.file === name)
      .map(event => event.action);
    const addedAncestor = events.some(event => {
      if (event.action !== nsfw.actions.CREATED) return false;
      const created = path.join(event.directory, event.file);
      return directory === created || directory.startsWith(created + path.sep);
    });
    assert.ok(addedAncestor, `No creation of ${directory} or its ancestors in ${JSON.stringify(events)}`);
    if (exists) {
      assert.ok(actions.lastIndexOf(nsfw.actions.CREATED) > actions.lastIndexOf(nsfw.actions.DELETED),
        JSON.stringify(actions));
    } else {
      assert.ok(!actions.includes(nsfw.actions.CREATED) ||
        actions.lastIndexOf(nsfw.actions.CREATED) < actions.lastIndexOf(nsfw.actions.DELETED),
      JSON.stringify(actions));
      if (knownBefore) {
        let lastCreation = -1;
        let lastDeletion = -1;
        events.forEach((event, index) => {
          const eventPath = path.join(event.directory, event.file || '');
          if (event.action === nsfw.actions.CREATED && eventPath === file) {
            lastCreation = index;
          } else if (event.action === nsfw.actions.DELETED &&
            (eventPath === file || file.startsWith(eventPath + path.sep))) {
            lastDeletion = index;
          }
        });
        assert.ok(lastDeletion > lastCreation, `No deletion of ${file} or its ancestors after its last creation in ` +
          JSON.stringify(events));
      }
    }
    const witness = 'after-return.file';
    await fsp.writeFile(path.join(directory, witness), 'visible');
    await waitFor(events, event => event.action === nsfw.actions.CREATED && event.directory === directory &&
      event.file === witness, 'creation after return');
  }

  // The slow build's hooks create this file once the event loop waits.
  const waitingPath = `/tmp/nsfw-test-waiting-${process.pid}`;

  // Creates start in the watched directory, so that the event loop waits until the file named end shows up, and
  // resolves once it does wait, after reading start on its own. Neither file may exist before. With the pause before
  // the lock, the read it waits in holds that one event, and nothing else is queued yet: a test that needs the queue
  // not to be empty once that read is handled (so that dropped watches aren't removed then) creates a file first,
  // before.file in most.
  async function untilLoopWaits(start, end) {
    await Promise.all([start, end].map(file => remove(path.join(watchedPath, file))));
    await remove(waitingPath);
    touch(start);
    for (let i = 0; i < 200 && !fs.existsSync(waitingPath); ++i) {
      await sleep(50);
    }
    assert.ok(fs.existsSync(waitingPath), `The event loop never waited after ${start}`);
  }

  // Everything operation causes is queued while the event loop is held, so it reads it in as few reads as fit.
  async function whileHeld(operation) {
    await untilLoopWaits('nsfw-test-hold-event-loop', 'nsfw-test-release-event-loop');
    await operation();
    touch('nsfw-test-release-event-loop');
  }

  // Created after the events under test, so once it's reported they have been handled. A new file each time, since
  // waitFor() looks at every event so far.
  let barriers = 0;
  async function barrier(events) {
    const file = `barrier-${barriers++}.file`;
    touch(file);
    await waitFor(events, event => event.action === nsfw.actions.CREATED && event.file === file, 'barrier');
  }

  beforeEach(async function() {
    // Node 22 runs no afterEach hook after a test calls t.skip().
    await remove(workDir);
    await mkdirp(watchedPath);
    await mkdirp(outsidePath);
  });

  // What gets reported for files written in `directory` once the events are handled.
  async function reportedIn(events, directory, files) {
    await barrier(events);
    for (const file of files) {
      await outputFile(path.join(directory, file), 'Written');
    }
    await sleep(1000);
    await barrier(events);
    return events
      .filter(event => event.action === nsfw.actions.CREATED)
      .filter(event => files.some(file => path.basename(file) === event.file))
      .map(event => path.relative(watchedPath, path.join(event.directory, event.file)));
  }

  // archive's scan builds a first, which the slow build takes 500 ms to do, then sees x.
  async function whileScanning(events, operation) {
    await whileHeld(async () => {
      await mkdirp(path.join(watchedPath, 'archive', 'a'));
      await fsp.rename(path.join(watchedPath, 'X'), path.join(watchedPath, 'archive', 'x'));
    });
    await waitFor(events, event => event.action === nsfw.actions.CREATED &&
      event.directory === path.join(watchedPath, 'archive') && event.file === 'x', 'scan of archive');
    await operation();
  }

  // Mounts need privileges, so these only run where mount works (as root, outside a container).
  async function withMount(test, mountArguments, mountPoint, run) {
    await mkdirp(mountPoint);
    try {
      cp.execFileSync('mount', [...mountArguments, mountPoint], { stdio: 'ignore' });
    } catch (error) {
      test.skip();
    }
    try {
      await run();
    } finally {
      try {
        cp.execFileSync('umount', [mountPoint], { stdio: 'ignore' });
      } catch (error) {
        // Still busy: detached, so the cleanup of the work directory doesn't go through it.
        cp.execFileSync('umount', ['-l', mountPoint], { stdio: 'ignore' });
      }
    }
  }

  // Written once the events are handled, so no scan finds the files, and named by directory, so a creation reported
  // under the wrong one doesn't count.
  async function expectWatched(events, ...directories) {
    for (const directory of directories) {
      const insideDirectory = path.join(watchedPath, ...directory);
      const prefix = `in-${directory.join('-')}-`;
      const reported = () => events.some(event => event.action === nsfw.actions.CREATED &&
        event.directory === insideDirectory && event.file.startsWith(prefix));
      for (let i = 0; i < 20 && !reported(); ++i) {
        await fsp.writeFile(path.join(insideDirectory, `${prefix}${i}.file`), 'Inside');
        await sleep(500);
      }
      assert.ok(reported(), `No creation in ${insideDirectory} in ${JSON.stringify(events)}`);
    }
  }
  before(function() {
    assert.ok(nsfw._native.NSFW_TEST_SLOW === true, 'NSFW should be built in slow mode');
  });
  afterEach(() => remove(workDir));
  after(() => remove(waitingPath));
  return {
    watchedPath, outsidePath, markerSignal, touch, watchWhile,
    waitFor, checkReturnState, waitingPath, untilLoopWaits, whileHeld,
    barrier, reportedIn, whileScanning, withMount, expectWatched
  };
};
