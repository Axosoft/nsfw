const { after, afterEach, before, beforeEach, describe } = require('node:test');
const assert = require('assert');
const fs = require('fs');
const fsp = require('fs/promises');
const os = require('os');
const path = require('path');
const vm = require('vm');
const { it, remove, waitUntil } = require('./common');

const nsfw = require('../dist');

const repoRoot = path.join(__dirname, '..', '..');
const defaultBinary = require('node-gyp-build').path(repoRoot);

describe('Native binding', function() {
  let copies;
  let directory;
  let savedEnvironment;
  let watchers;

  // Each copy is a separate file, so it is loaded as an addon of its own, whose instance count tells which addon
  // created a watcher.
  function copyBinary(name) {
    const file = path.join(copies, name, 'nsfw.node');
    fs.mkdirSync(path.dirname(file));
    fs.copyFileSync(defaultBinary, file);
    return file;
  }

  async function assertWatches(options) {
    const events = [];
    const watcher = await nsfw(directory, batch => events.push(...batch), { debounceMS: 100, ...options });
    await watcher.start();
    try {
      const file = `created-${events.length}-${Date.now()}.file`;
      await fsp.writeFile(path.join(directory, file), 'created');
      await waitUntil(() => events.some(event => event.file === file),
        { message: () => `No creation of ${file} in ${JSON.stringify(events)}` });
    } finally {
      await watcher.stop();
    }
    // Collecting a watcher lowers its addon's instance count, which the tests assert after this returns.
    watchers.push(watcher);
  }

  before(async function() {
    copies = await fsp.mkdtemp(path.join(os.tmpdir(), 'nsfw-binding-'));
  });

  after(async function() {
    // Windows keeps a loaded addon's file open until the process exits.
    await remove(copies).catch(() => {});
  });

  beforeEach(async function() {
    directory = await fsp.mkdtemp(path.join(os.tmpdir(), 'nsfw-binding-watch-'));
    watchers = [];
    savedEnvironment = process.env.NSFW_NATIVE_BINDING;
    delete process.env.NSFW_NATIVE_BINDING;
  });

  afterEach(async function() {
    if (savedEnvironment === undefined) {
      delete process.env.NSFW_NATIVE_BINDING;
    } else {
      process.env.NSFW_NATIVE_BINDING = savedEnvironment;
    }
    await remove(directory);
  });

  it('loads the addon from the nativeBinding path', async function() {
    const file = copyBinary('option-path');
    await assertWatches({ nativeBinding: file });
    const addon = require(file);
    assert.notStrictEqual(addon, nsfw._native);
    assert.strictEqual(addon.getAllocatedInstanceCount(), 1);
  });

  it('uses a nativeBinding addon that was already loaded', async function() {
    const addon = require(copyBinary('option-object'));
    await assertWatches({ nativeBinding: addon });
    assert.strictEqual(addon.getAllocatedInstanceCount(), 1);
  });

  it('loads the addon from NSFW_NATIVE_BINDING', async function() {
    const file = copyBinary('environment');
    process.env.NSFW_NATIVE_BINDING = file;
    await assertWatches({});
    assert.strictEqual(require(file).getAllocatedInstanceCount(), 1);
  });

  it('prefers the nativeBinding option over NSFW_NATIVE_BINDING', async function() {
    const environmentFile = copyBinary('overridden');
    const optionFile = copyBinary('overriding');
    process.env.NSFW_NATIVE_BINDING = environmentFile;
    await assertWatches({ nativeBinding: optionFile });
    assert.strictEqual(require(optionFile).getAllocatedInstanceCount(), 1);
    assert.strictEqual(require(environmentFile).getAllocatedInstanceCount(), 0);

    process.env.NSFW_NATIVE_BINDING = path.join(copies, 'missing', 'nsfw.node');
    await assertWatches({ nativeBinding: optionFile });
  });

  it('runs watchers of different addons side by side', async function() {
    const first = copyBinary('first');
    const second = copyBinary('second');
    const events = [[], []];
    const watchers = await Promise.all([first, second].map((nativeBinding, index) =>
      nsfw(directory, batch => events[index].push(...batch), { debounceMS: 100, nativeBinding })));
    await Promise.all(watchers.map(watcher => watcher.start()));
    try {
      await fsp.writeFile(path.join(directory, 'shared.file'), 'shared');
      await waitUntil(() => events.every(batch => batch.some(event => event.file === 'shared.file')),
        { message: () => `Not reported by both: ${JSON.stringify(events)}` });
    } finally {
      await Promise.all(watchers.map(watcher => watcher.stop()));
    }
    assert.strictEqual(require(first).getAllocatedInstanceCount(), 1);
    assert.strictEqual(require(second).getAllocatedInstanceCount(), 1);
  });

  it('rejects a nativeBinding that is not an absolute .node path nor the addon', async function() {
    const invalid = [42, '', null, {}, true, 'nsfw.node', path.join(copies, 'nsfw.js'), class {}, nsfw];
    fs.writeFileSync(path.join(copies, 'nsfw.js'), 'throw new Error("ran as JavaScript");');
    for (const nativeBinding of invalid) {
      await assert.rejects(nsfw(directory, () => {}, { nativeBinding }),
        { name: 'TypeError', message: /nativeBinding must be/ }, String(nativeBinding));
    }
  });

  it('rejects an NSFW_NATIVE_BINDING that is not an absolute .node path', async function() {
    for (const value of ['nsfw.node', path.join(copies, 'nsfw.js')]) {
      process.env.NSFW_NATIVE_BINDING = value;
      await assert.rejects(nsfw(directory, () => {}),
        { name: 'TypeError', message: /NSFW_NATIVE_BINDING must be/ }, value);
    }
  });

  it('rejects a missing binding file, naming it', async function() {
    const missing = path.join(copies, 'missing', 'nsfw.node');
    const expected = error => error.message.includes(`${missing}: no such file`);
    await assert.rejects(nsfw(directory, () => {}, { nativeBinding: missing }), expected);
    process.env.NSFW_NATIVE_BINDING = missing;
    await assert.rejects(nsfw(directory, () => {}), expected);
  });

  it('does not load what require() would try in place of a missing binding file', async function() {
    const script = 'throw new Error("ran as JavaScript");';
    const folder = path.join(copies, 'folder.node');
    fs.mkdirSync(folder);
    fs.writeFileSync(path.join(folder, 'index.js'), script);
    const sibling = path.join(copies, 'sibling.node');
    fs.writeFileSync(`${sibling}.js`, script);
    for (const nativeBinding of [folder, sibling]) {
      await assert.rejects(nsfw(directory, () => {}, { nativeBinding }),
        error => error.message.endsWith(`${nativeBinding}: no such file`), nativeBinding);
    }
  });

  // Runs index.js as if it were installed in root.
  function loadFrom(root) {
    const module = { exports: {} };
    vm.runInNewContext(fs.readFileSync(path.join(__dirname, '../dist/index.js'), 'utf8'), {
      module,
      __dirname: path.join(root, 'js', 'dist'),
      process,
      require
    });
    return module.exports;
  }

  it('can be imported where it cannot find its addon', async function() {
    const isolated = loadFrom(copies);
    assert.strictEqual(isolated.actions.RENAMED, 3);
    await assert.rejects(isolated(directory, () => {}),
      { message: /could not find its native binding.*nativeBinding option or the NSFW_NATIVE_BINDING/s });
    const watcher = await isolated(directory, () => {}, { nativeBinding: copyBinary('isolated') });
    await watcher.start();
    await watcher.stop();
  });

  it('tells a binary that fails to load from a missing one', async function() {
    const root = path.join(copies, 'corrupt');
    fs.mkdirSync(path.join(root, 'build', 'Release'), { recursive: true });
    const file = path.join(root, 'build', 'Release', 'nsfw.node');
    fs.writeFileSync(file, 'not a binary');
    await assert.rejects(loadFrom(root)(directory, () => {}),
      error => error.message.startsWith(`nsfw could not load its native binding from ${file}: `) &&
        error.cause.code === 'ERR_DLOPEN_FAILED');
  });
});
