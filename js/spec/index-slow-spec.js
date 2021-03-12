const assert = require('assert');
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
});
