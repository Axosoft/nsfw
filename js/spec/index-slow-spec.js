const { after, afterEach, beforeEach, describe } = require('node:test');
const assert = require('assert');
const fs = require('fs');
const fsp = require('fs/promises');
const path = require('path');

const {
  DEBOUNCE, WORKDIR: workDir, countInotifyWatches, eventually, it, mkdirp, readDescriptors, remove, sleep
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

  const touch = file => fs.closeSync(fs.openSync(path.join(workDir, file), 'w'));

  async function createWatcher(watchPath = workDir, options = {}) {
    let resolveError;
    const errored = new Promise(resolve => { resolveError = resolve; });
    const events = [];
    const watch = await nsfw(watchPath, batch => events.push(...batch),
      { debounceMS: DEBOUNCE, errorCallback: resolveError, ...options });
    return { watch, errored, events };
  }

  async function expectError(errored, timeout) {
    const error = await Promise.race([errored, sleep(timeout).then(() => null)]);
    assert.ok(error instanceof Error, `errorCallback was not called within ${timeout} ms`);
    return error.message;
  }

  // The slow build has test hooks in the Linux backend, triggered by files with these names in the watched directory.
  // FreeBSD builds the same backend on libinotify, which these hooks weren't written for and has no /proc limit.
  describe('inotify errors', { skip: process.platform !== 'linux' }, function() {
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

  // The slow build injects the events FSEvents sends when it loses some, as the events of files with these names.
  describe('FSEvents lost events', { skip: process.platform !== 'darwin' }, function() {
    const dropped = 'FSEvents dropped events, events were lost';
    const coalesced = 'FSEvents coalesced the events of a directory tree, events were lost';

    // The events injected come before the one of file, so an error they caused would come by then, or soon after.
    async function expectNoError(errored, events, file) {
      const failed = errored.then(error => error.message);
      const reported = eventually(() => events.some(event => event.file === file), { timeout: 10000 });
      assert.strictEqual(await Promise.race([failed, reported]), true, `no event for ${file}, or an error`);
      assert.strictEqual(await Promise.race([failed, sleep(DEBOUNCE * 5).then(() => null)]), null);
    }

    async function expectStopped(options, file, message) {
      const { watch, errored } = await createWatcher(workDir, options);
      try {
        await watch.start();
        touch(file);
        assert.strictEqual(await expectError(errored, 10000), message);
        await assert.rejects(watch.stop(), /not running/);
      } finally {
        await watch.stop().catch(() => {});
      }
    }

    it('reports dropped events and stops the watcher', async function() {
      await expectStopped({}, 'nsfw-test-drop-events', dropped);
    });

    it('reports dropped events in an excluded directory', async function() {
      await fsp.mkdir(path.join(workDir, 'excluded'));
      await expectStopped({ excludedPatterns: ['/excluded'] }, 'excluded/nsfw-test-drop-events', dropped);
    });

    it('reports events coalesced in the watched directory and stops the watcher', async function() {
      await expectStopped({}, 'nsfw-test-coalesce-events', coalesced);
    });

    it('reports events coalesced in a directory below the watched one', async function() {
      await fsp.mkdir(path.join(workDir, 'sub'));
      await expectStopped({}, 'sub/nsfw-test-coalesce-events', coalesced);
    });

    it('reports dropped events whatever their id', async function() {
      await expectStopped({}, 'nsfw-test-drop-events-before-start', dropped);
    });

    it('ignores events coalesced before the watcher started', async function() {
      const { watch, errored, events } = await createWatcher();
      try {
        await watch.start();
        touch('nsfw-test-coalesce-events-before-start');
        touch('reported');
        await expectNoError(errored, events, 'reported');
      } finally {
        await watch.stop().catch(() => {});
      }
    });

    it('ignores events coalesced in an excluded or ignored directory', async function() {
      await fsp.mkdir(path.join(workDir, 'excluded', 'deeper'), { recursive: true });
      await fsp.mkdir(path.join(workDir, 'ignored', 'deeper'), { recursive: true });
      const { watch, errored, events } = await createWatcher(workDir, {
        excludedPaths: [path.join(workDir, 'excluded')],
        excludedPatterns: ['ignored/']
      });
      try {
        await watch.start();
        touch('excluded/nsfw-test-coalesce-events');
        touch('excluded/deeper/nsfw-test-coalesce-events');
        touch('ignored/nsfw-test-coalesce-events');
        touch('ignored/deeper/nsfw-test-coalesce-events');
        touch('reported');
        await expectNoError(errored, events, 'reported');
      } finally {
        await watch.stop().catch(() => {});
      }
    });

    // A watched file recovers from any failure of its native watcher, so what that watcher reports can only be seen on
    // one built here with the options a watched file uses, see createFileWatcher() in js/src/index.ts.
    function createFileNativeWatcher(name) {
      let resolveError;
      const errored = new Promise(resolve => { resolveError = resolve; });
      const events = [];
      const watch = new nsfw._native(workDir, batch => events.push(...batch), {
        debounceMS: DEBOUNCE,
        errorCallback: resolveError,
        excludedPaths: [],
        excludedPatterns: ['/*', `!/${name}`, `/${name}/*`],
        respectGitignore: false,
        followFileSymlinks: false,
        recursive: false
      });
      return { watch, errored, events };
    }

    it('ignores, for a watched file, events coalesced below its directory', async function() {
      await fsp.mkdir(path.join(workDir, 'sub'));
      touch('watched');
      const { watch, errored, events } = createFileNativeWatcher('watched');
      try {
        await watch.start();
        touch('sub/nsfw-test-coalesce-events');
        await fsp.appendFile(path.join(workDir, 'watched'), 'changed');
        await expectNoError(errored, events, 'watched');
      } finally {
        await watch.stop().catch(() => {});
      }
    });

    it('reports, for a watched file, events coalesced in its directory', async function() {
      touch('watched');
      const { watch, errored } = createFileNativeWatcher('watched');
      try {
        await watch.start();
        touch('nsfw-test-coalesce-events');
        assert.strictEqual(await expectError(errored, 10000), coalesced);
      } finally {
        await watch.stop().catch(() => {});
      }
    });

    it('recovers, for a watched file, from events coalesced in its directory', async function() {
      touch('watched');
      const { watch, errored, events } = await createWatcher(path.join(workDir, 'watched'));
      try {
        await watch.start();
        // Fails its native watcher, as the spec above shows; the directory is watched again from the next whole second.
        touch('nsfw-test-coalesce-events');
        await sleep(2500);
        await fsp.appendFile(path.join(workDir, 'watched'), 'changed');
        await expectNoError(errored, events, 'watched');
      } finally {
        await watch.stop().catch(() => {});
      }
    });
  });
});
