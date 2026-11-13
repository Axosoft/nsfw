const assert = require('assert');
const cp = require('child_process');
const fse = require('fs-extra');
const os = require('os');
const path = require('path');

describe('Directory watcher lifecycle recovery', function() {
  this.timeout(25000);
  let directory;

  beforeEach(async function() {
    directory = await fse.mkdtemp(path.join(os.tmpdir(), 'nsfw-lifecycle-'));
  });

  afterEach(async function() {
    await fse.remove(directory);
  });

  function runChild(mode) {
    return new Promise((resolve, reject) => {
      const child = cp.spawn(process.execPath, [
        '--expose-gc', path.join(__dirname, 'lifecycle-child.js'), mode, directory
      ], { stdio: 'inherit' });
      const timer = setTimeout(() => {
        child.kill();
        reject(new Error(`child process (${mode}) did not exit on its own`));
      }, 20000);
      child.on('error', error => {
        clearTimeout(timer);
        reject(error);
      });
      child.on('exit', (code, signal) => {
        clearTimeout(timer);
        resolve({ code, signal });
      });
    });
  }

  it('runs unawaited lifecycle operations in call order, including after rejected jobs', async function() {
    assert.deepEqual(await runChild('ordered-jobs'), { code: 0, signal: null });
  });

  it('can restart directly inside the error callback and receive events on the new run', async function() {
    assert.deepEqual(await runChild('restart-after-error'), { code: 0, signal: null });
  });

  it('tears down workers with pending lifecycle jobs', async function() {
    assert.deepEqual(await runChild('terminate-queued'), { code: 0, signal: null });
  });

  it('collects errored native watchers and callbacks that capture their own watcher', async function() {
    assert.deepEqual(await runChild('collect-errored'), { code: 0, signal: null });
  });
});
