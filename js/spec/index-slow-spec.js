const { after, afterEach, beforeEach, describe } = require('node:test');
const assert = require('assert');
const fs = require('fs');
const fsp = require('fs/promises');
const path = require('path');

const {
  DEBOUNCE, WORKDIR: workDir, countInotifyWatches, it, mkdirp, readDescriptors, remove, sleep
} = require('./common');

const nsfw = require('../dist');

describe('Node Sentinel File Watcher (slow)', function() {

  assert.ok(nsfw._native.NSFW_TEST_SLOW === true, 'NSFW should be built in slow mode');

  // Node 22 runs no afterEach hook after a test calls t.skip().
  beforeEach(async function() {
    await remove(workDir);
    await fsp.mkdir(workDir, { recursive: true });
  });

  afterEach(function() {
    return remove(workDir);
  });

  // Created by the hooks of the slow build once the event loop waits, see untilLoopWaits().
  after(function() {
    return remove(`/tmp/nsfw-test-waiting-${process.pid}`);
  });

  it('can listen for the creation of a deeply nested file', async function () {
    const folders = 'a_very_deep_tree'.split(''); // ['a', '_', ...]
    const file = 'a_file.txt';

    // Resolve a promise as soon as all events have been found
    let done; const promise = new Promise(resolve => {
      done = resolve;
    });

    /**
     * We will remove or pop entries as we find them.
     * This set should be empty at the end of the test.
     * @type {Set<string>}
     */
    const events = new Set();
    let directory = workDir; for (const folder of folders) {
      directory = path.join(directory, folder);
      events.add(directory);
    }
    const filePath = path.join(directory, file);
    events.add(filePath);

    function findEvent(element) {
      if (element.action === nsfw.actions.CREATED) {
        const file = path.join(element.directory, element.file);
        if (events.has(file)) {
          events.delete(file);
          if (events.size === 0) {
            done();
          }
        }
      }
    }

    let watch = await nsfw(
      workDir,
      events => events.forEach(findEvent),
      { debounceMS: DEBOUNCE }
    );

    try {
      await watch.start();
      await mkdirp(directory);
      await fsp.writeFile(filePath, '');
      await Promise.race([
        sleep(folders.length * 500 * 1.5),
        promise,
      ]);

      // Make sure that we got the events for each nested directory
      assert.ok(events.size === 0, `Some files were not detected:\n${
        Array.from(events, file => `- ${file}`).join('\n')
      }`);
    } finally {
      await watch.stop();
      watch = null;
    }
  });

  // The slow build has test hooks in the Linux backend, triggered by files with these names in the watched directory.
  // FreeBSD builds the same backend on libinotify, which these hooks weren't written for and has no /proc limit.
  describe('inotify errors', { skip: process.platform !== 'linux' }, function() {
    const touch = file => fs.closeSync(fs.openSync(path.join(workDir, file), 'w'));

    async function createWatcher() {
      let resolveError;
      const errored = new Promise(resolve => { resolveError = resolve; });
      const watch = await nsfw(workDir, () => {}, { debounceMS: DEBOUNCE, errorCallback: resolveError });
      return { watch, errored };
    }

    async function expectError(errored, timeout) {
      const error = await Promise.race([errored, sleep(timeout).then(() => null)]);
      assert.ok(error instanceof Error, `errorCallback was not called within ${timeout} ms`);
      return error.message;
    }

    function maxQueuedEventsOrSkip(test) {
      const maxQueuedEvents = Number(fs.readFileSync('/proc/sys/fs/inotify/max_queued_events', 'utf8'));
      if (!(maxQueuedEvents <= 50000)) {
        // Too many files to create in a test run, and the hold file gives up after a minute.
        test.skip();
      }
      return maxQueuedEvents;
    }

    // The hold file pauses the event loop, so these events stay in the kernel queue. The loop may have read up to 8 KB
    // of them (a few hundred) along with the hold file, hence the margin.
    function overflowWhileHeld(maxQueuedEvents) {
      touch('nsfw-test-hold-event-loop');
      for (let i = 0; i < maxQueuedEvents + 1000; i++) {
        touch(`file${i}`);
      }
    }

    it('reports a kernel event queue overflow and stops the watcher', async function() {
      const maxQueuedEvents = maxQueuedEventsOrSkip(this);
      const { watch, errored } = await createWatcher();
      try {
        await watch.start();
        overflowWhileHeld(maxQueuedEvents);
        touch('nsfw-test-release-event-loop');

        assert.strictEqual(await expectError(errored, 30000), 'Inotify event queue overflowed, events were lost');
        await assert.rejects(watch.stop(), /not running/);
      } finally {
        // Lets stop() through if the flood threw while the event loop was held.
        touch('nsfw-test-release-event-loop');
        await watch.stop().catch(() => {});
      }
    });

    it('reports a deleted root after a queue overflow', async function() {
      const maxQueuedEvents = maxQueuedEventsOrSkip(this);
      const { watch, errored } = await createWatcher();
      try {
        await watch.start();
        overflowWhileHeld(maxQueuedEvents);
        // The hold file lets the loop go once the directory is gone. Whichever it records first, the missing root or
        // the overflow it reads, getError() reports the root path error over it.
        await remove(workDir);

        assert.strictEqual(
          await expectError(errored, 30000),
          'Service shutdown: root path changed (renamed or deleted)'
        );
        await assert.rejects(watch.stop(), /not running/);
      } finally {
        await watch.stop().catch(() => {});
      }
    });

    it('reports a failed read and stops the watcher', async function() {
      const { watch, errored } = await createWatcher();
      try {
        await watch.start();
        touch('nsfw-test-fail-read');

        assert.strictEqual(await expectError(errored, 10000), 'Inotify read failed: Bad file descriptor');
        await assert.rejects(watch.stop(), /not running/);
      } finally {
        await watch.stop().catch(() => {});
      }
    });

    it('reports through errorCallback a failure that stops the event loop before start() checks it', async function() {
      // With this file in the watched directory, the slow build makes the loop's first read fail and waits for the loop
      // to stop before start() checks that the watcher runs.
      touch('nsfw-test-fail-first-read');

      const { watch, errored } = await createWatcher();
      try {
        await watch.start();

        assert.strictEqual(await expectError(errored, 10000), 'Inotify read failed: Bad file descriptor');
        await assert.rejects(watch.stop(), /not running/);
      } finally {
        await watch.stop().catch(() => {});
      }
    });
  });

  // Linux only, like the inotify errors above.
  describe('Inotify descriptor', { skip: process.platform !== 'linux' }, function() {
    it('leaves a descriptor that took its number alone when the root fails to watch', async function() {
      // With this file next to the root, the slow build opens it right after closing the inotify descriptor of a root
      // that failed to watch, as another thread could have, so it gets the number of that descriptor.
      const root = path.join(workDir, 'removed');
      // As the descriptors show it: nsfw() resolves the root's path.
      const marker = path.join(fs.realpathSync(workDir), 'nsfw-test-take-closed-inotify');
      await fsp.writeFile(marker, '');
      await fsp.mkdir(root);
      const markerDescriptors = () => readDescriptors(fd => [fd, fs.readlinkSync(`/proc/self/fd/${fd}`)])
        .filter(([, target]) => target === marker).map(([fd]) => fd);
      const watch = await nsfw(root, () => {}, { debounceMS: DEBOUNCE });
      await remove(root);
      try {
        await assert.rejects(watch.start(), /unable to start watching/);

        assert.strictEqual(markerDescriptors().length, 1, 'The descriptor that took the closed number was closed');
      } finally {
        markerDescriptors().forEach(fd => fs.closeSync(fd));
        await watch.stop().catch(() => {});
      }
    });

    it('releases the descriptor and its watches when starting throws', async function() {
      // With this file in the root, the slow build throws once the tree holds its watches, as running out of memory
      // there would.
      const root = path.join(workDir, 'throwing');
      await mkdirp(path.join(root, 'a', 'b'));
      await fsp.writeFile(path.join(root, 'nsfw-test-throw-before-loop'), '');
      const countInstances = () => readDescriptors(fd => fs.readlinkSync(`/proc/self/fd/${fd}`))
        .filter(target => target === 'anon_inode:inotify').length;
      const watch = await nsfw(root, () => {}, { debounceMS: DEBOUNCE });
      const instances = countInstances();
      const watches = countInotifyWatches();

      try {
        await assert.rejects(watch.start(), /unable to start watching/);

        assert.strictEqual(countInstances(), instances, 'The inotify descriptor leaked');
        assert.strictEqual(countInotifyWatches(), watches, 'The inotify watches leaked');
      } finally {
        await watch.stop().catch(() => {});
      }
    });
  });

});
