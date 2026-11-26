const { afterEach, beforeEach, describe } = require('node:test');
const assert = require('assert');
const fsp = require('fs/promises');
const path = require('path');

const { WORKDIR: workDir, barrier, it, remove, untilNextSecond, waitForEvent } = require('./common');

const nsfw = require('../dist');

const { CREATED, DELETED, MODIFIED, RENAMED } = nsfw.actions;
const { FILE } = nsfw.kinds;
// The watchers that list the tree when they start find the symlinks already there.
const listsTree = process.platform === 'linux' || process.platform === 'freebsd';
// FSEvents can report a symlink that appeared as modified: it tells a modification by the times of what it leads to.
const APPEARED = process.platform === 'darwin' ? undefined : CREATED;

describe('Symlinked files', function() {
  const inPath = path.join(workDir, 'tree');
  const outPath = path.join(workDir, 'outside');
  const target = path.join(inPath, 'target.file');
  let events;
  let watch;

  const isAt = (event, file, action) => event.directory === inPath && event.file === file &&
    (action === undefined || event.action === action);
  const waitFor = (file, action, name) => waitForEvent(events, event => isAt(event, file, action),
    `${name} of ${file}`);
  const reportedUnder = file => events.filter(event => isAt(event, file));
  // What is reported under a symlink is about the symlink itself, which is no directory.
  const assertSymlinkKind = file => assert.deepStrictEqual(reportedUnder(file).filter(event => event.kind !== FILE), [],
    JSON.stringify(events));
  // The event of an entry that appeared itself, not one reported under a symlink.
  const waitForAppearance = file => waitFor(file, APPEARED, 'creation');

  async function watchTree(options = {}) {
    watch = await nsfw(inPath, batch => events.push(...batch.map(({ action, kind, directory, file }) =>
      ({ action, kind, directory, file }))), { debounceMS: 50, ...options });
    await watch.start();
  }

  // Waits until everything that happened so far is reported, then starts recording afresh.
  async function settle() {
    await barrier(events, inPath);
    events.length = 0;
  }

  async function modify(file) {
    await untilNextSecond(file);
    await fsp.appendFile(file, 'more');
  }

  beforeEach(async function() {
    events = [];
    await remove(workDir);
    await fsp.mkdir(inPath, { recursive: true });
    await fsp.mkdir(outPath, { recursive: true });
    await fsp.writeFile(target, 'target');
  });

  afterEach(async function() {
    if (watch) await watch.stop();
    watch = null;
    await remove(workDir);
  });

  it('reports a change of the target under a symlink created while watching', async function() {
    await watchTree();
    await fsp.symlink('target.file', path.join(inPath, 'link'), 'file');
    await waitForAppearance('link');
    await settle();

    await modify(target);
    await waitFor('target.file', MODIFIED, 'modification');
    await waitFor('link', MODIFIED, 'modification');
    const order = events.map(event => event.file);
    assert.ok(order.indexOf('target.file') < order.indexOf('link'), JSON.stringify(events));
    assertSymlinkKind('link');
  });

  // A walk in the background finds the symlinks already there at some point after start(): changes the target until
  // one is reported under the symlink, as isReported tells.
  async function modifyUntilReported(isReported) {
    for (let attempt = 0; attempt < 20; ++attempt) {
      events.length = 0;
      await modify(target);
      await waitFor('target.file', MODIFIED, 'modification');
      await barrier(events, inPath);
      if (events.some(isReported)) return true;
    }
    return false;
  }

  it('follows the symlinks that were there when it started with followFileSymlinks', async function() {
    const nested = path.join(inPath, 'nested', 'deeper');
    await fsp.mkdir(nested, { recursive: true });
    await fsp.symlink(path.join('..', '..', 'target.file'), path.join(nested, 'link'), 'file');
    await fsp.symlink('target.file', path.join(inPath, 'link'), 'file');
    await watchTree({ followFileSymlinks: true });
    assert.ok(await modifyUntilReported(event => event.directory === nested && event.file === 'link'),
      JSON.stringify(events));
    await waitFor('link', MODIFIED, 'modification');
  });

  it('follows the symlinks that were there when it started without followFileSymlinks only on Linux',
    async function() {
      await fsp.symlink('target.file', path.join(inPath, 'link'), 'file');
      await watchTree();
      await modify(target);
      await waitFor('target.file', MODIFIED, 'modification');
      if (listsTree) {
        await waitFor('link', MODIFIED, 'modification');
      } else if (process.platform === 'win32') {
        await barrier(events, inPath);
        assert.deepStrictEqual(reportedUnder('link'), []);
      }
      // FSEvents can still deliver the symlink's own creation, made just before start(), which makes it followed.
    });

  it('rejects a followFileSymlinks that is not a boolean', async function() {
    await assert.rejects(() => nsfw(inPath, () => {}, { followFileSymlinks: 'yes' }),
      /^TypeError: followFileSymlinks must be a boolean/);
  });

  it('reports a change made through the symlink under both paths', async function() {
    await watchTree();
    const link = path.join(inPath, 'link');
    await fsp.symlink(target, link, 'file');
    await waitForAppearance('link');
    await settle();

    await modify(link);
    await waitFor('target.file', MODIFIED, 'modification');
    await waitFor('link', MODIFIED, 'modification');
  });

  it('reports the symlink deleted and created again with its target', async function() {
    await watchTree();
    await fsp.symlink('target.file', path.join(inPath, 'link'), 'file');
    await waitForAppearance('link');
    await settle();

    await fsp.unlink(target);
    await waitFor('link', DELETED, 'removal');
    await settle();
    await fsp.writeFile(target, 'again');
    await waitFor('link', CREATED, 'creation');
    await settle();

    await fsp.rename(target, path.join(inPath, 'moved.file'));
    await waitFor('link', DELETED, 'removal');
    await settle();
    await fsp.rename(path.join(inPath, 'moved.file'), target);
    await waitFor('link', CREATED, 'creation');
    assertSymlinkKind('link');
  });

  it('follows a symlink retargeted, renamed or removed', async function() {
    const other = path.join(inPath, 'other.file');
    await fsp.writeFile(other, 'other');
    await watchTree();
    const link = path.join(inPath, 'link');
    await fsp.symlink('target.file', link, 'file');
    await waitForAppearance('link');

    await fsp.unlink(link);
    await fsp.symlink('other.file', link, 'file');
    await settle();
    await modify(target);
    await modify(other);
    await waitFor('link', MODIFIED, 'modification');
    await waitFor('other.file', MODIFIED, 'modification');
    await waitFor('target.file', MODIFIED, 'modification');
    // The target's change came first: had it been reported under the symlink, it would be by now.
    assert.strictEqual(reportedUnder('link').length, 1, JSON.stringify(events));

    await fsp.rename(link, path.join(inPath, 'renamed'));
    await settle();
    await modify(other);
    await waitFor('renamed', MODIFIED, 'modification');
    assert.deepStrictEqual(reportedUnder('link'), []);

    await fsp.unlink(path.join(inPath, 'renamed'));
    await settle();
    await modify(other);
    await waitFor('other.file', MODIFIED, 'modification');
    await barrier(events, inPath);
    assert.deepStrictEqual(reportedUnder('renamed'), []);
  });

  it('reports through a chain of symlinks and ignores a cycle', async function() {
    await watchTree();
    await fsp.symlink('target.file', path.join(inPath, 'first'), 'file');
    await fsp.symlink('first', path.join(inPath, 'second'), 'file');
    await fsp.symlink('loop-b', path.join(inPath, 'loop-a'), 'file');
    await fsp.symlink('loop-a', path.join(inPath, 'loop-b'), 'file');
    await waitForAppearance('loop-b');
    await settle();

    await modify(target);
    await waitFor('first', MODIFIED, 'modification');
    await waitFor('second', MODIFIED, 'modification');

    // The middle link going away leaves the last one dangling.
    await settle();
    await fsp.unlink(path.join(inPath, 'first'));
    await waitFor('second', DELETED, 'removal');
  });

  it('follows symlinks and targets in a renamed directory', async function() {
    const directory = path.join(inPath, 'dir');
    await fsp.mkdir(directory);
    await fsp.writeFile(path.join(directory, 'inner.file'), 'inner');
    await watchTree();
    await fsp.symlink(path.join('..', 'target.file'), path.join(directory, 'up'), 'file');
    await fsp.symlink(path.join('dir', 'inner.file'), path.join(inPath, 'down'), 'file');
    await waitForAppearance('down');
    await settle();

    // The symlink inside goes along; the one leading into it now dangles.
    await fsp.rename(directory, path.join(inPath, 'moved'));
    await waitFor('down', DELETED, 'removal');
    await settle();
    await modify(target);
    await waitForEvent(events, event => event.directory === path.join(inPath, 'moved') && event.file === 'up' &&
      event.action === MODIFIED, 'modification of moved/up');
  });

  it('follows symlinks in a directory renamed to another case', async function(t) {
    const directory = path.join(inPath, 'Dir');
    await fsp.mkdir(directory);
    let caseInsensitive = true;
    try {
      await fsp.access(path.join(inPath, 'dir'));
    } catch {
      caseInsensitive = false;
    }
    if (!caseInsensitive) {
      t.skip('the filesystem tells names apart by case');
      return;
    }
    await watchTree();
    await fsp.symlink(path.join('..', 'target.file'), path.join(directory, 'link'), 'file');
    await waitForEvent(events, event => event.file === 'link' && (APPEARED === undefined || event.action === APPEARED),
      'creation of link');
    await fsp.rename(directory, path.join(inPath, 'dir'));
    await settle();
    await modify(target);
    await waitForEvent(events, event => event.directory.toLowerCase() === path.join(inPath, 'dir').toLowerCase() &&
      event.file === 'link' && event.action === MODIFIED, 'modification of dir/link');
  });

  it('follows a target in a directory a symlink to it is listed before', async function() {
    // Listed first, the symlink is where the Linux tree watches the directory, and reports its changes.
    const real = path.join(inPath, 'z-real');
    await fsp.mkdir(real);
    const file = path.join(real, 'inner.file');
    await fsp.writeFile(file, 'inner');
    await fsp.symlink(real, path.join(inPath, 'a-alias'), 'dir');
    await watchTree();
    await fsp.symlink(path.join('z-real', 'inner.file'), path.join(inPath, 'link'), 'file');
    await waitForAppearance('link');
    await settle();
    await modify(file);
    await waitFor('link', MODIFIED, 'modification');
    if (listsTree) {
      // Else the tree held the directory at its own path, and this didn't test what it meant to.
      assert.ok(events.some(event => event.directory === path.join(inPath, 'a-alias') && event.file === 'inner.file'),
        JSON.stringify(events));
    }
  });

  describe('in a directory a symlink to it is listed before', function() {
    // Listed first, the alias is where the Linux tree watches the real directory, and reports its entries.
    const real = path.join(inPath, 'z-real');
    const isLink = (event, action) => event.file === 'file-link' && (action === undefined || event.action === action);
    const waitForLink = (action, name) => waitForEvent(events, event => isLink(event, action), `${name} of file-link`);
    const linkDirectories = () => new Set(events.filter(event => isLink(event)).map(event => event.directory));

    async function makeAlias(directory = inPath) {
      await fsp.mkdir(real);
      await fsp.mkdir(directory, { recursive: true });
      await fsp.symlink(real, path.join(directory, 'a-alias'), 'dir');
    }

    const makeLink = () => fsp.symlink(path.join('..', 'target.file'), path.join(real, 'file-link'), 'file');

    // The changes of the target, then the removal of the symlink, under the same path.
    async function assertModifiedAndRemovedUnderOnePath() {
      await modify(target);
      await waitForLink(MODIFIED, 'modification');
      await fsp.unlink(path.join(real, 'file-link'));
      await waitForLink(DELETED, 'removal');
      await barrier(events, inPath);
      assert.strictEqual(linkDirectories().size, 1, JSON.stringify(events));
    }

    it('reports a symlink created there under one path', async function() {
      await makeAlias();
      await watchTree();
      await makeLink();
      await waitForLink(APPEARED, 'creation');
      await barrier(events, inPath);
      await assertModifiedAndRemovedUnderOnePath();
      if (listsTree) {
        // Else the tree held the directory at its own path, and this didn't test what it meant to.
        assert.ok(linkDirectories().has(path.join(inPath, 'a-alias')), JSON.stringify(events));
      }
    });

    it('reports a symlink there when it started under one path', async function() {
      await makeAlias();
      await makeLink();
      await watchTree({ followFileSymlinks: true });
      assert.ok(await modifyUntilReported(event => isLink(event, MODIFIED)), JSON.stringify(events));
      await settle();
      await assertModifiedAndRemovedUnderOnePath();
    });

    const waitForRoot = action => waitForEvent(events, event => event.directory === inPath && event.action === action,
      'change of the alias');

    for (const [change, act, action] of [
      ['removed', () => fsp.unlink(path.join(inPath, 'a-alias')), DELETED],
      ['renamed', () => fsp.rename(path.join(inPath, 'a-alias'), path.join(inPath, 'b-alias')), RENAMED],
      ['repointed', async () => {
        const other = path.join(inPath, 'y-real');
        await fsp.mkdir(other);
        // Windows doesn't rename a symlink to a directory over another.
        if (process.platform === 'win32') {
          await fsp.unlink(path.join(inPath, 'a-alias'));
          await fsp.symlink(other, path.join(inPath, 'a-alias'), 'dir');
          return;
        }
        await fsp.symlink(other, path.join(inPath, 'new-alias'), 'dir');
        await fsp.rename(path.join(inPath, 'new-alias'), path.join(inPath, 'a-alias'));
      }, process.platform === 'win32' ? CREATED : RENAMED],
    ]) {
      it(`reports a symlink there under one path once the alias is ${change}`, async function() {
        await makeAlias();
        await watchTree();
        await makeLink();
        await waitForLink(APPEARED, 'creation');
        await act();
        await waitForRoot(action);
        await settle();
        await assertModifiedAndRemovedUnderOnePath();
      });
    }

    it('reports a symlink there under one path once the directory holding the alias is renamed', async function() {
      await makeAlias(path.join(inPath, 'outer'));
      await watchTree();
      await makeLink();
      await waitForLink(APPEARED, 'creation');
      await fsp.rename(path.join(inPath, 'outer'), path.join(inPath, 'outer2'));
      await waitForRoot(RENAMED);
      await settle();
      await assertModifiedAndRemovedUnderOnePath();
    });
  });

  it('follows a chain retargeted in the middle by what is on disk', async function() {
    await watchTree();
    await fsp.symlink('middle', path.join(inPath, 'first'), 'file');
    await fsp.symlink('target.file', path.join(inPath, 'middle'), 'file');
    await waitForAppearance('middle');
    await settle();

    // The middle now leads nowhere: the first one dangles, and appears again with what it leads to.
    await fsp.unlink(path.join(inPath, 'middle'));
    await fsp.symlink('missing.file', path.join(inPath, 'middle'), 'file');
    await waitFor('first', DELETED, 'removal');
    await settle();
    await fsp.writeFile(path.join(inPath, 'missing.file'), 'there');
    await waitFor('first', CREATED, 'creation');
    await barrier(events, inPath);
    // Once: what it leads to appearing is reported as such, not taken for granted earlier.
    assert.deepStrictEqual(reportedUnder('first').filter(event => event.action !== MODIFIED).map(event => event.action),
      [CREATED], JSON.stringify(events));
  });

  it('reports a target saved by renaming another file over it as modified', async function() {
    await watchTree();
    await fsp.symlink('target.file', path.join(inPath, 'link'), 'file');
    await waitForAppearance('link');
    const saved = path.join(inPath, 'saved.tmp');
    await fsp.writeFile(saved, 'saved');
    await waitFor('saved.tmp', undefined, 'change');
    await settle();

    await fsp.rename(saved, target);
    // Windows reports the file replaced as removed, then the rename: so is the symlink.
    const replaced = process.platform === 'win32' ? [DELETED, CREATED] : [];
    await waitFor('link', process.platform === 'win32' ? CREATED : MODIFIED, 'change');
    await barrier(events, inPath);
    assert.deepStrictEqual(reportedUnder('link').filter(event => event.action !== MODIFIED).map(event => event.action),
      replaced, JSON.stringify(events));
  });

  it('follows a symlink to a directory in the path of the target', async function() {
    const real = path.join(inPath, 'real');
    await fsp.mkdir(real);
    await fsp.writeFile(path.join(real, 'inner.file'), 'inner');
    await watchTree();
    await fsp.symlink('real', path.join(inPath, 'via'), 'dir');
    await fsp.symlink(path.join('via', 'inner.file'), path.join(inPath, 'link'), 'file');
    await waitForAppearance('link');
    await settle();

    // The symlink to the directory goes: the file is still there, but the symlink to it dangles.
    await fsp.unlink(path.join(inPath, 'via'));
    await waitFor('link', DELETED, 'removal');
    await settle();
    // Back again, the symlink leads to the file again: nothing reports what is below a symlink to a directory.
    await fsp.symlink('real', path.join(inPath, 'via'), 'dir');
    await waitFor('link', CREATED, 'creation');
    await settle();
    await modify(path.join(real, 'inner.file'));
    await waitFor('link', MODIFIED, 'modification');
  });

  it('reports the symlink created once when a directory holding its target is moved in', async function() {
    const away = path.join(outPath, 'dir');
    await fsp.mkdir(away);
    await fsp.writeFile(path.join(away, 'inner.file'), 'inner');
    await watchTree();
    await fsp.symlink(path.join('dir', 'inner.file'), path.join(inPath, 'link'), 'file');
    await waitForAppearance('link');
    await settle();

    await fsp.rename(away, path.join(inPath, 'dir'));
    await waitFor('link', CREATED, 'creation');
    await barrier(events, inPath);
    assert.deepStrictEqual(reportedUnder('link').map(event => event.action), [CREATED], JSON.stringify(events));
    assertSymlinkKind('link');
  });

  it('stops reporting under a symlink whose directory gets excluded', async function() {
    const directory = path.join(inPath, 'dir');
    await fsp.mkdir(directory);
    await watchTree();
    await fsp.symlink(path.join('..', 'target.file'), path.join(directory, 'link'), 'file');
    await waitForEvent(events, event => event.directory === directory && event.file === 'link', 'creation of link');
    await watch.updateExcludedPaths([directory]);
    await settle();
    await modify(target);
    await waitFor('target.file', MODIFIED, 'modification');
    await barrier(events, inPath);
    assert.deepStrictEqual(events.filter(event => event.directory === directory), [], JSON.stringify(events));
  });

  it('reports a target removed with its directory and created again once each, in order', async function() {
    const directory = path.join(inPath, 'dir');
    await fsp.mkdir(directory);
    await fsp.writeFile(path.join(directory, 'inner.file'), 'inner');
    await watchTree();
    await fsp.symlink(path.join('dir', 'inner.file'), path.join(inPath, 'link'), 'file');
    await waitForAppearance('link');
    await settle();

    await remove(directory);
    await fsp.mkdir(directory);
    await fsp.writeFile(path.join(directory, 'inner.file'), 'again');
    await waitForEvent(events, event => event.directory === directory && event.file === 'inner.file',
      'change of inner.file');
    await barrier(events, inPath);
    // As the target is reported, which FSEvents can merge into modifications, each once and after the target's.
    const actions = list => list.map(event => event.action).filter((action, index, all) =>
      action !== all[index - 1]);
    const target = events.filter(event => event.directory === directory && event.file === 'inner.file');
    assert.deepStrictEqual(actions(reportedUnder('link')), actions(target), JSON.stringify(events));
    if (process.platform !== 'darwin') {
      assert.deepStrictEqual(actions(reportedUnder('link')).filter(action => action !== MODIFIED), [DELETED, CREATED],
        JSON.stringify(events));
    }
    const order = events.map(event => event.file);
    assert.ok(order.lastIndexOf('inner.file') < order.lastIndexOf('link'), JSON.stringify(events));
    assertSymlinkKind('link');
  });

  it('follows a symlink to a directory nested in the path of the target', async function() {
    const outer = path.join(inPath, 'outer');
    const deep = path.join(inPath, 'deep');
    await fsp.mkdir(outer);
    await fsp.mkdir(deep);
    await fsp.writeFile(path.join(deep, 'inner.file'), 'inner');
    await fsp.symlink(path.join('..', 'deep'), path.join(outer, 'nested'), 'dir');
    await watchTree();
    await fsp.symlink('outer', path.join(inPath, 'via'), 'dir');
    await fsp.symlink(path.join('via', 'nested', 'inner.file'), path.join(inPath, 'link'), 'file');
    await waitForAppearance('link');
    await settle();

    await fsp.unlink(path.join(outer, 'nested'));
    await waitFor('link', DELETED, 'removal');
  });

  it('keeps following symlinks while paused', async function() {
    await watchTree();
    await watch.pause();
    await fsp.symlink('target.file', path.join(inPath, 'link'), 'file');
    await untilNextSecond(path.join(inPath, 'target.file'));
    await watch.resume();
    await settle();
    await modify(target);
    await waitFor('link', MODIFIED, 'modification');
  });

  it('does not report under a symlink to a file outside the watched path', async function() {
    const outside = path.join(outPath, 'outside.file');
    await fsp.writeFile(outside, 'outside');
    await watchTree();
    await fsp.symlink(outside, path.join(inPath, 'link'), 'file');
    await waitForAppearance('link');
    await settle();

    await modify(outside);
    await modify(target);
    await waitFor('target.file', MODIFIED, 'modification');
    await barrier(events, inPath);
    assert.deepStrictEqual(reportedUnder('link'), []);
  });

  it('does not report under an excluded symlink, or one leading to an excluded file', async function() {
    const excluded = path.join(inPath, 'excluded');
    await fsp.mkdir(excluded);
    const hidden = path.join(inPath, 'hidden.file');
    await fsp.writeFile(hidden, 'hidden');
    await fsp.symlink('../target.file', path.join(excluded, 'there'), 'file');
    await watchTree({ excludedPaths: [excluded], excludedPatterns: ['hidden.file'], followFileSymlinks: true });
    await fsp.symlink(target, path.join(excluded, 'link'), 'file');
    await fsp.symlink('hidden.file', path.join(inPath, 'to-hidden'), 'file');
    await fsp.symlink('target.file', path.join(inPath, 'kept'), 'file');
    await waitForAppearance('kept');
    await settle();

    await modify(hidden);
    await modify(target);
    await waitFor('kept', MODIFIED, 'modification');
    await barrier(events, inPath);
    assert.deepStrictEqual(reportedUnder('to-hidden'), []);
    assert.deepStrictEqual(events.filter(event => event.directory === excluded), []);
  });

  it('follows a symlink that was there when it started once the patterns no longer ignore it', async function() {
    const nested = path.join(inPath, 'nested');
    await fsp.mkdir(nested);
    await fsp.symlink('target.file', path.join(inPath, 'link'), 'file');
    await fsp.symlink(path.join('..', 'target.file'), path.join(nested, 'link'), 'file');
    await fsp.symlink('target.file', path.join(inPath, 'kept'), 'file');
    await watchTree({ excludedPatterns: ['link'], followFileSymlinks: true });
    assert.ok(await modifyUntilReported(event => isAt(event, 'kept', MODIFIED)), JSON.stringify(events));
    assert.deepStrictEqual(events.filter(event => event.file === 'link'), []);

    await watch.updateExcludedPatterns([]);
    assert.ok(await modifyUntilReported(event => event.directory === nested && event.file === 'link'),
      JSON.stringify(events));
    await waitFor('link', MODIFIED, 'modification');
    assertSymlinkKind('link');
  });
});
