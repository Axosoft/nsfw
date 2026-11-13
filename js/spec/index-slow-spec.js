const assert = require('assert');
const fs = require('fs');
const fse = require('fs-extra');
const path = require('path');

const { DEBOUNCE, TIMEOUT_PER_STEP, WORKDIR: workDir, sleep } = require('./common');

const nsfw = require('../src');

describe('Node Sentinel File Watcher (slow)', function() {
  this.timeout(120000);

  assert.ok(nsfw._native.NSFW_TEST_SLOW === true, 'NSFW should be built in slow mode');

  beforeEach(function() {
    return fse.mkdir(workDir, { recursive: true });
  });

  afterEach(function() {
    return fse.remove(workDir);
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
      await sleep(TIMEOUT_PER_STEP);
      await fse.mkdirp(directory);
      await fse.close(await fse.open(filePath, 'w'));
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
  describe('inotify errors', function() {
    before(function() {
      // FreeBSD builds the same backend on libinotify, which these hooks weren't written for and has no /proc limit.
      if (process.platform !== 'linux') {
        this.skip();
      }
    });

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
        // The hold file lets the loop go once the directory is gone. Whichever comes first, the poll thread noticing
        // the missing root or the loop reading the overflow, the root path error is the one reported.
        await fse.remove(workDir);

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
});
