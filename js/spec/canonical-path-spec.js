const assert = require('assert');
const fs = require('fs');
const path = require('path');
const vm = require('vm');

const source = fs.readFileSync(path.join(__dirname, '../dist/index.js'), 'utf8');

function platformWrapper(platform, files = {}, methods = {}) {
  let native;
  class Native {
    constructor(watchPath, callback, options) {
      native = { watchPath, callback, excludedPaths: options.excludedPaths };
    }
    async getExcludedPaths() { return native.excludedPaths; }
    async updateExcludedPaths(paths) { native.excludedPaths = paths; return paths; }
  }
  Object.assign(Native.prototype, methods);
  const module = { exports: {} };
  vm.runInNewContext(source, {
    module,
    process: { platform, emitWarning() {} },
    require(name) {
      if (name === 'path') return platform === 'win32' ? path.win32 : path.posix;
      if (name === 'fs') return { promises: {
        stat: async () => ({ dev: global.BigInt(1), ino: global.BigInt(1), isDirectory: () => true }), ...files
      } };
      return Native;
    }
  });
  return { nsfw: module.exports, native: () => native };
}

const windowsWrapper = () => platformWrapper('win32');

describe('Linux public directory spelling', function() {
  for (const [input, root, nested] of [
    ['/watch', '/watch', '/watch/child'],
    ['/watch/', '/watch', '/watch/child'],
    ['/watch//', '/watch/', '/watch//child'],
    ['/watch//./', '/watch//.', '/watch//./child'],
    ['/', '/', '/child'],
    ['//', '/', '//child']
  ]) {
    it(`retains the legacy root and descendant spelling of ${input}`, async function() {
      const wrapper = platformWrapper('linux', { realpath: async value => path.posix.resolve(value) });
      let events;
      await wrapper.nsfw(input, batch => { events = batch; });
      const canonical = wrapper.native().watchPath;
      wrapper.native().callback([{ directory: canonical, newDirectory: path.posix.join(canonical, 'child') }]);
      assert.strictEqual(events[0].directory, root);
      assert.strictEqual(events[0].newDirectory, nested);
    });
  }
});

describe('Canonical POSIX root identity at the native boundary', function() {
  it('requires physical proof for case variants across volume boundaries', async function() {
    const root = '/sensitive/Mount/watch';
    const realpaths = new Map([
      ['/', '/'], ['/sensitive', '/sensitive'], ['/sensitive/Mount', '/sensitive/Mount'],
      [root, root], ['/sensitive/Mount/WATCH', root]
    ]);
    const read = async value => {
      if (!realpaths.has(value)) throw Object.assign(new Error('Missing path'), { code: 'ENOENT' });
      return realpaths.get(value);
    };
    const wrapper = platformWrapper('darwin', { realpath: read });
    const watch = await wrapper.nsfw(root, () => {},
      { excludedPaths: ['/sensitive/mount/watch', '/sensitive/Mount/WATCH/future'] });
    assert.deepStrictEqual(Array.from(await watch.getExcludedPaths()), [root + '/future']);
    assert.deepStrictEqual(Array.from(await watch.updateExcludedPaths(['/sensitive/mount/watch'])), []);
    assert.deepStrictEqual(Array.from(await watch.updateExcludedPaths(['/sensitive/Mount/WATCH'])), [root]);
  });

  it('ignores an alias whose kernel lookup disagrees with realpath', async function() {
    const wrapper = platformWrapper('darwin', {
      realpath: async value => value === '/alias' ? '/root' : value,
      stat: async value => ({
        dev: global.BigInt(1), ino: global.BigInt(value === '/alias' ? 2 : 1), isDirectory: () => true
      })
    });
    const watch = await wrapper.nsfw('/root', () => {}, { excludedPaths: ['/alias/future'] });
    assert.deepStrictEqual(Array.from(await watch.getExcludedPaths()), []);
    assert.deepStrictEqual(Array.from(await watch.updateExcludedPaths(['/alias'])), []);
  });
});

describe('Canonical POSIX exclusions under a known root', function() {
  it('are resolved without reading the filesystem', async function() {
    const reads = [];
    const wrapper = platformWrapper('linux', {
      async realpath(value) {
        reads.push(value);
        return value.replace(/^\/alias\/\.\.\/root/, '/root');
      }
    });
    const watch = await wrapper.nsfw('/alias/../root/', () => {}, { excludedPaths: ['/alias/../root/a'] });
    const construction = reads.length;
    assert.deepStrictEqual(Array.from(await watch.getExcludedPaths()), ['/root/a']);
    assert.deepStrictEqual(Array.from(await watch.updateExcludedPaths([
      '/alias/..//root/./b/', '/root/c//d/.', '/root', '/alias/../root/'
    ])), ['/root/b', '/root/c/d', '/root', '/root']);
    assert.strictEqual(reads.length, construction);
  });
});

describe('Canonical Windows paths at the native boundary', function() {
  for (const [input, nativeRoot, metadataRoot] of [
    ['C:/', '\\\\?\\C:\\', 'C:\\'],
    ['\\\\?\\C:\\', '\\\\?\\C:\\', 'C:\\'],
    ['\\\\server\\share\\', '\\\\?\\UNC\\server\\share\\', '\\\\?\\UNC\\server\\share\\']
  ]) {
    it('uses compatible metadata spelling while preserving native root ' + input, async function() {
      const reads = [];
      const wrapper = platformWrapper('win32', {
        async stat(value) {
          reads.push(value);
          if (/^\\\\\?\\[A-Za-z]:\\$/.test(value)) {
            throw Object.assign(new Error('Extended drive root'), { code: 'EISDIR' });
          }
          return { isDirectory: () => true };
        }
      });
      const watch = await wrapper.nsfw(input, () => {}, { excludedPaths: [input] });
      assert.deepStrictEqual(reads, [metadataRoot]);
      assert.strictEqual(wrapper.native().watchPath, nativeRoot);
      assert.deepStrictEqual(Array.from(await watch.getExcludedPaths()), [nativeRoot]);
    });
  }

  for (const [input, canonical] of [
    ['C:/watch//./', '\\\\?\\C:\\watch'],
    ['C:\\', '\\\\?\\C:\\'],
    ['\\\\server\\share\\', '\\\\?\\UNC\\server\\share\\'],
    ['\\\\?\\UNC\\server\\share\\', '\\\\?\\UNC\\server\\share\\']
  ]) {
    it(`canonicalizes ${input} and preserves roots`, async function() {
      const wrapper = windowsWrapper();
      const watch = await wrapper.nsfw(input, () => {}, { excludedPaths: [input, input + 'future\\'] });
      assert.strictEqual(wrapper.native().watchPath, canonical);
      assert.deepStrictEqual(Array.from(await watch.getExcludedPaths()),
        [canonical, path.win32.join(canonical, 'future')]);
      const paths = await watch.updateExcludedPaths([path.win32.join(input, 'future'), 'Z:\\outside']);
      assert.deepStrictEqual(Array.from(paths), [path.win32.join(canonical, 'future')]);
    });
  }

  it('maps both rename directories to the public Windows spelling', async function() {
    const wrapper = windowsWrapper();
    let events;
    await wrapper.nsfw('C:/watch/./', batch => { events = batch; });
    wrapper.native().callback([{ directory: '\\\\?\\C:\\watch', newDirectory: '\\\\?\\C:\\watch\\sub' }]);
    assert.strictEqual(events[0].directory, 'C:\\watch\\');
    assert.strictEqual(events[0].newDirectory, 'C:\\watch\\sub');
  });

  it('retains an explicit namespace and trailing root separator in public events', async function() {
    const wrapper = windowsWrapper();
    let events;
    await wrapper.nsfw('\\\\?\\C:\\watch\\', batch => { events = batch; });
    wrapper.native().callback([{ directory: '\\\\?\\C:\\watch', newDirectory: '\\\\?\\C:\\watch\\sub' }]);
    assert.strictEqual(events[0].directory, '\\\\?\\C:\\watch\\');
    assert.strictEqual(events[0].newDirectory, '\\\\?\\C:\\watch\\sub');
  });
});


describe('Watcher call submission order', function() {
  it('submits stop after canonicalization without waiting for native update completion', async function() {
    const calls = [];
    let complete, readPath, releasePath;
    const reading = new Promise(resolve => { readPath = resolve; });
    const canonicalizing = new Promise(resolve => { releasePath = resolve; });
    const completion = new Promise(resolve => { complete = resolve; });
    const wrapper = platformWrapper('linux', {
      async realpath(value) {
        if (value === '/alias') {
          readPath();
          await canonicalizing;
          return '/root/child';
        }
        return value;
      }
    }, {
      updateExcludedPaths(paths) { calls.push(['update', paths]); return completion; },
      async stop() { calls.push(['stop']); complete(['/root/child']); }
    });
    const watch = await wrapper.nsfw('/root', () => {});
    const update = watch.updateExcludedPaths(['/alias']);
    await reading;
    const stop = watch.stop();
    assert.deepStrictEqual(calls, []);
    releasePath();
    await stop;
    assert.deepStrictEqual(Array.from(await update), ['/root/child']);
    assert.strictEqual(calls[0][0], 'update');
    assert.deepStrictEqual(Array.from(calls[0][1]), ['/root/child']);
    assert.deepStrictEqual(calls[1], ['stop']);
  });
});
