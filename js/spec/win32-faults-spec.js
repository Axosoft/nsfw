const { afterEach, beforeEach, describe } = require('node:test');
const assert = require('assert');
const fsp = require('fs/promises');
const path = require('path');

const { WORKDIR: workDir, it, mkdirp, remove, waitForEvent, waitUntil } = require('./common');

const nsfw = require('../dist');

const KiB = 1024;
const bufferBytes = () => nsfw._native.getReadBufferBytes();

describe('Windows read buffers', function() {
  assert.ok(nsfw._native.NSFW_TEST_FAULTS === true, 'NSFW should be built with NSFW_TEST_FAULTS');

  const directory = path.join(workDir, 'watched');
  let savedFault;
  let watchers;

  async function watch(watchPath) {
    const events = [];
    const errors = [];
    const watcher = await nsfw(watchPath, batch => events.push(...batch), {
      debounceMS: 50,
      errorCallback: error => errors.push(error)
    });
    watchers.push(watcher);
    await watcher.start();
    return { events, errors };
  }

  async function assertReports({ events, errors }) {
    const file = `created-${Date.now()}.file`;
    await fsp.writeFile(path.join(directory, file), 'created');
    await waitForEvent(events, event => event.file === file,
      `creation of ${file} (errors ${JSON.stringify(errors.map(String))})`);
    assert.deepStrictEqual(errors, []);
  }

  beforeEach(async function() {
    savedFault = process.env.NSFW_TEST_READ_FAULT;
    delete process.env.NSFW_TEST_READ_FAULT;
    watchers = [];
    // The watchers of the tests before are released, whether by stop() or once collected.
    await waitUntil(() => {
      global.gc();
      return bufferBytes() === 0;
    }, { message: () => `Read buffers still hold ${bufferBytes()} bytes` });
    await remove(workDir);
    await mkdirp(directory);
  });

  afterEach(async function() {
    if (savedFault === undefined) {
      delete process.env.NSFW_TEST_READ_FAULT;
    } else {
      process.env.NSFW_TEST_READ_FAULT = savedFault;
    }
    // One that failed is stopped already, and its stop() rejects.
    await Promise.allSettled(watchers.map(watcher => watcher.stop()));
    await remove(workDir);
  });

  it('retries with a smaller buffer when a share rejects the read at once', async function() {
    process.env.NSFW_TEST_READ_FAULT = 'network-sync';
    await assertReports(await watch(directory));
  });

  it('retries with a smaller buffer when a share fails the read on completion', async function() {
    process.env.NSFW_TEST_READ_FAULT = 'network-async';
    const watched = await watch(directory);
    // The failure comes once started, and changes before the read is issued again are missed: the retry allocates its
    // buffer right before issuing it.
    await waitUntil(() => bufferBytes() === 64 * KiB, { message: () => `Read buffers hold ${bufferBytes()} bytes` });
    await assertReports(watched);
  });

  it('reports the failure of a read that is already as small as a share takes', async function() {
    process.env.NSFW_TEST_READ_FAULT = 'later-async';
    const { errors } = await watch(directory);
    // The first read gets it and the next ones fail, which may drop its event.
    await fsp.writeFile(path.join(directory, 'created.file'), 'created');
    await waitUntil(() => errors.length > 0, { message: 'No error reported' });
    assert.match(String(errors[0]), /Service shutdown unexpectedly: .*\(Windows error 87\)/);
  });

  it('releases the larger buffer after shrinking it', async function() {
    process.env.NSFW_TEST_READ_FAULT = 'network-sync';
    const watched = await watch(directory);
    assert.strictEqual(bufferBytes(), 64 * KiB);
    // The second buffer, once a read completes, is as small.
    await assertReports(watched);
    assert.strictEqual(bufferBytes(), 128 * KiB);
  });

  it('allocates one buffer until the first read completes', async function() {
    await watch(directory);
    assert.strictEqual(bufferBytes(), 1024 * KiB);
  });

  it('gives a watched file a small buffer', async function() {
    const file = path.join(directory, 'watched.file');
    await fsp.writeFile(file, 'watched');
    await watch(file);
    assert.strictEqual(bufferBytes(), 64 * KiB);
  });
});
