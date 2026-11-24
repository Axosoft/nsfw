const { describe } = require('node:test');
const assert = require('assert');
const cp = require('child_process');
const fs = require('fs');
const fsp = require('fs/promises');
const path = require('path');
const nsfw = require('../../dist');
const { countInotifyWatches, it, mkdirp, remove, sleep, watchCountReaches } = require('../common');
const fixture = require('./fixture');

describe('inotify moves (slow)', { skip: process.platform !== 'linux' }, function() {
  const {
    watchedPath, outsidePath, touch, watchWhile, waitFor,
    checkReturnState, untilLoopWaits, whileHeld, barrier, expectWatched
  } = fixture();


  it('stops watching a directory moved out in the same read as the events after it', async function() {
    await mkdirp(path.join(watchedPath, 'leaving'));
    touch('existing.file');

    await watchWhile(async events => {
      await whileHeld(async () => {
        await fsp.rename(path.join(watchedPath, 'leaving'), path.join(outsidePath, 'leaving'));
        await fsp.appendFile(path.join(watchedPath, 'existing.file'), 'Modified');
      });
      await waitFor(events, event => event.action === nsfw.actions.MODIFIED && event.file === 'existing.file',
        'modification');
      await fsp.writeFile(path.join(outsidePath, 'leaving', 'outside.file'), 'Outside');
      await barrier(events);

      assert.ok(events.some(event => event.action === nsfw.actions.DELETED && event.file === 'leaving'));
      const created = file => events.filter(event => event.action === nsfw.actions.CREATED && event.file === file);
      assert.deepStrictEqual({
        outside: events.filter(event => event.file === 'outside.file'),
        // The modification after the move is not reported as a creation too.
        existingCreated: created('existing.file')
      }, { outside: [], existingCreated: [] });
    });
  });

  it('watches what moves back in from a directory that left in the same read', async function() {
    await mkdirp(path.join(watchedPath, 'leaving', 'inner'));
    touch(path.join('leaving', 'inner.file'));

    await watchWhile(async events => {
      await whileHeld(async () => {
        await fsp.rename(path.join(watchedPath, 'leaving'), path.join(outsidePath, 'leaving'));
        await fsp.rename(path.join(outsidePath, 'leaving', 'inner'), path.join(watchedPath, 'back'));
        await fsp.rename(path.join(outsidePath, 'leaving', 'inner.file'), path.join(watchedPath, 'back.file'));
      });
      const created = file => event => event.action === nsfw.actions.CREATED && event.directory === watchedPath &&
        event.file === file;
      await waitFor(events, created('back'), 'directory moved back');
      await waitFor(events, created('back.file'), 'file moved back');

      const insidePath = path.join(watchedPath, 'back', 'inside.file');
      await fsp.writeFile(insidePath, 'Inside');
      await waitFor(events, event => event.action === nsfw.actions.CREATED &&
        event.directory === path.dirname(insidePath) && event.file === 'inside.file', 'creation inside');
    });
  });

  it('reports failure to restore a watch after a directory returns', async function() {
    await mkdirp(path.join(watchedPath, 'single'));
    let resolveError;
    const errored = new Promise(resolve => { resolveError = resolve; });
    const watch = await nsfw(watchedPath, () => {}, { debounceMS: 100, errorCallback: resolveError });
    try {
      await watch.start();
      await whileHeld(async () => {
        await fsp.rename(path.join(watchedPath, 'single'), path.join(outsidePath, 'single'));
        await fsp.rename(path.join(outsidePath, 'single'), path.join(watchedPath, 'back'));
        touch('nsfw-test-fail-rewatch');
      });
      const error = await Promise.race([errored, sleep(10000).then(() => null)]);
      assert.ok(error instanceof Error, 'errorCallback was not called');
      assert.strictEqual(error.message, 'Kernel out of memory');
    } finally {
      touch('nsfw-test-release-event-loop');
      await watch.stop().catch(() => {});
    }
  });

  it('watches a directory moved out and back in within one read with the watches it had', async function() {
    await mkdirp(path.join(watchedPath, 'round', 'trip'));
    const otherWatches = countInotifyWatches();

    await watchWhile(async events => {
      // The watched path, round and trip.
      assert.strictEqual(countInotifyWatches(), otherWatches + 3);
      await whileHeld(async () => {
        await fsp.rename(path.join(watchedPath, 'round'), path.join(outsidePath, 'round'));
        await fsp.rename(path.join(outsidePath, 'round'), path.join(watchedPath, 'back'));
      });
      const insidePath = path.join(watchedPath, 'back', 'trip', 'inside.file');
      await fsp.writeFile(insidePath, 'Inside');
      await waitFor(events, event => event.action === nsfw.actions.CREATED &&
        event.directory === path.dirname(insidePath) && event.file === 'inside.file', 'creation inside');
      // The removal of the watches it had when it left doesn't remove the ones it has now.
      await barrier(events);
      assert.strictEqual(countInotifyWatches(), otherWatches + 3);
    });
  });

  it('watches a directory renamed over an empty watched one in the read that emptied it', async function() {
    await mkdirp(path.join(watchedPath, 'source', 'inner'));
    await mkdirp(path.join(watchedPath, 'target', 'sub'));
    const otherWatches = countInotifyWatches();

    await watchWhile(async events => {
      assert.strictEqual(countInotifyWatches(), otherWatches + 5);
      await whileHeld(async () => {
        await fsp.rename(path.join(watchedPath, 'target', 'sub'), path.join(watchedPath, 'sub'));
        await fsp.rename(path.join(watchedPath, 'source'), path.join(watchedPath, 'target'));
      });
      await expectWatched(events, ['target'], ['target', 'inner'], ['sub']);
      await barrier(events);
      assert.strictEqual(countInotifyWatches(), otherWatches + 4);
    });
  });

  it('stops watching what a symlink led to once a directory is renamed over it in the same read', async function() {
    await mkdirp(path.join(outsidePath, 'linked', 'below'));
    await mkdirp(path.join(watchedPath, 'source', 'inner'));
    await fsp.symlink(path.join(outsidePath, 'linked'), path.join(watchedPath, 'target'));
    const otherWatches = countInotifyWatches();

    await watchWhile(async events => {
      assert.strictEqual(countInotifyWatches(), otherWatches + 5);
      await whileHeld(async () => {
        await fsp.unlink(path.join(watchedPath, 'target'));
        await fsp.mkdir(path.join(watchedPath, 'target'));
        await fsp.rename(path.join(watchedPath, 'source'), path.join(watchedPath, 'target'));
      });
      await expectWatched(events, ['target'], ['target', 'inner']);
      await fsp.writeFile(path.join(outsidePath, 'linked', 'outside.file'), 'Outside');
      await barrier(events);
      assert.deepStrictEqual(events.filter(event => event.file === 'outside.file'), []);
      // The directory the symlink led to still exists, so the kernel won't drop its watch: the tree has to.
      await watchCountReaches(otherWatches + 3);
    });
  });

  it('watches a directory moved out of the one a removed symlink led to, once that one is deleted', async function() {
    await mkdirp(path.join(outsidePath, 'linked', 'inner', 'below'));
    await fsp.symlink(path.join(outsidePath, 'linked'), path.join(watchedPath, 'target'));
    const otherWatches = countInotifyWatches();

    await watchWhile(async events => {
      assert.strictEqual(countInotifyWatches(), otherWatches + 4);
      await fsp.unlink(path.join(watchedPath, 'target'));
      await barrier(events);
      await fsp.rename(path.join(outsidePath, 'linked', 'inner'), path.join(watchedPath, 'target'));
      await expectWatched(events, ['target'], ['target', 'below']);
      await fsp.writeFile(path.join(outsidePath, 'linked', 'outside.file'), 'Outside');
      await remove(path.join(outsidePath, 'linked'));
      await barrier(events);
      assert.deepStrictEqual(events.filter(event => event.file === 'outside.file'), []);
      const afterPath = path.join(watchedPath, 'target', 'below', 'after.file');
      await fsp.writeFile(afterPath, 'After');
      await waitFor(events, event => event.action === nsfw.actions.CREATED &&
        event.directory === path.dirname(afterPath) && event.file === 'after.file', 'creation after the deletion');
      await watchCountReaches(otherWatches + 3);
    });
  });

  for (const [departure, destination] of [
    ['moved out', path.join(outsidePath, 'z-gone')],
    ['renamed onto an excluded path', path.join(watchedPath, 'z-new')]
  ]) {
    it(`stops watching a directory held at a symlink once it is renamed and ${departure} in the same read`,
      async function() {
        await mkdirp(path.join(watchedPath, 'z-real'));
        await fsp.symlink('z-real', path.join(watchedPath, 'a-link'));
        const otherWatches = countInotifyWatches();

        await watchWhile(async events => {
          assert.strictEqual(countInotifyWatches(), otherWatches + 2);
          await whileHeld(async () => {
            await fsp.rename(path.join(watchedPath, 'z-real'), path.join(watchedPath, 'z-other'));
            await fsp.rename(path.join(watchedPath, 'z-other'), destination);
          });
          await barrier(events);
          await fsp.writeFile(path.join(destination, 'left.file'), 'Left');
          await barrier(events);
          assert.deepStrictEqual(events.filter(event => event.file === 'left.file'), []);
          await watchCountReaches(otherWatches + 1);
        }, [path.join(watchedPath, 'z-new')]);
      });
  }

  // Node has no renameat2(): mv --exchange (coreutils 9.5), or Python's ctypes.
  const exchange = (from, to) => [
    ['mv', ['--exchange', '--no-target-directory', from, to]],
    ['python3', ['-c', 'import ctypes, sys; AT_FDCWD, RENAME_EXCHANGE = -100, 2; ' +
      'libc = ctypes.CDLL(None, use_errno=True); sys.exit(libc.renameat2(' +
      'AT_FDCWD, sys.argv[1].encode(), AT_FDCWD, sys.argv[2].encode(), RENAME_EXCHANGE) != 0)', from, to]]
  ].some(([command, args]) => cp.spawnSync(command, args).status === 0);

  // Where it's reported isn't checked: what the tree reports after an exchange is known to be misattributed.
  it('keeps reporting a directory held at a symlink below one exchanged with another', async function(t) {
    const probe = [path.join(outsidePath, 'probe-a'), path.join(outsidePath, 'probe-b')];
    await Promise.all(probe.map(directory => mkdirp(directory)));
    if (!exchange(...probe)) {
      return t.skip();
    }
    await mkdirp(path.join(watchedPath, 'z-real', 'z'));
    await mkdirp(path.join(watchedPath, 'w'));
    await fsp.symlink(path.join('z-real', 'z'), path.join(watchedPath, 'a-link'));

    await watchWhile(async events => {
      assert.ok(exchange(path.join(watchedPath, 'w'), path.join(watchedPath, 'z-real')));
      await barrier(events);
      await fsp.writeFile(path.join(watchedPath, 'w', 'z', 'held.file'), 'Held');
      await waitFor(events, event => event.action === nsfw.actions.CREATED && event.file === 'held.file',
        'creation in the held directory');
    });
  });

  // Only that no watch is left behind: what the tree reports after an exchange is known to be misattributed.
  it('frees the watches of directories exchanged with each other', async function(t) {
    const probe = [path.join(outsidePath, 'probe-a'), path.join(outsidePath, 'probe-b')];
    await Promise.all(probe.map(directory => mkdirp(directory)));
    if (!exchange(...probe)) {
      return t.skip();
    }
    await mkdirp(path.join(watchedPath, 'first', 'below-first'));
    await mkdirp(path.join(watchedPath, 'second', 'below-second'));
    const otherWatches = countInotifyWatches();

    await watchWhile(async events => {
      assert.strictEqual(countInotifyWatches(), otherWatches + 5);
      assert.ok(exchange(path.join(watchedPath, 'first'), path.join(watchedPath, 'second')));
      await barrier(events);
      // Moved out, neither is watched anymore.
      await fsp.rename(path.join(watchedPath, 'first'), path.join(outsidePath, 'first'));
      await fsp.rename(path.join(watchedPath, 'second'), path.join(outsidePath, 'second'));
      await barrier(events);
      await watchCountReaches(otherWatches + 1);
    });
  });

  // With no other dropped watch left to remove, the marker after a directory found again is the removal of its own
  // watch: it's watched again with a new one, which is the one its scan after the marker has to use.
  it('watches a directory created outside in one that comes back with the last dropped watch', async function() {
    await mkdirp(path.join(watchedPath, 'D'));

    await watchWhile(async events => {
      await whileHeld(async () => {
        fs.renameSync(path.join(watchedPath, 'D'), path.join(outsidePath, 'D'));
        fs.mkdirSync(path.join(outsidePath, 'D', 'new'));
        fs.renameSync(path.join(outsidePath, 'D'), path.join(watchedPath, 'D'));
      });
      await barrier(events);
      await expectWatched(events, ['D', 'new']);
    });
  });

  // The scan of archive finds moving there before the move's events are handled.
  for (const [description, operation] of [
    ['', async () => {
      await fsp.rename(path.join(watchedPath, 'moving'), path.join(watchedPath, 'archive', 'moving'));
    }],
    [' and a symlink takes its place', async () => {
      await fsp.rename(path.join(watchedPath, 'moving'), path.join(watchedPath, 'archive', 'moving'));
      await fsp.symlink(path.join('archive', 'moving'), path.join(watchedPath, 'moving'));
    }],
    [' through an excluded path', async () => {
      await fsp.rename(path.join(watchedPath, 'moving'), path.join(watchedPath, 'excluded'));
      await fsp.rename(path.join(watchedPath, 'excluded'), path.join(watchedPath, 'archive', 'moving'));
    }]
  ]) {
    it(`watches a directory moved into a directory created in the same read${description}`, async function() {
      await mkdirp(path.join(watchedPath, 'moving', 'inner'));
      const otherWatches = countInotifyWatches();

      await watchWhile(async events => {
        await whileHeld(async () => {
          await mkdirp(path.join(watchedPath, 'archive'));
          await operation();
        });
        const insidePath = path.join(watchedPath, 'archive', 'moving', 'inner', 'inside.file');
        await fsp.writeFile(insidePath, 'Inside');
        await waitFor(events, event => event.action === nsfw.actions.CREATED &&
          event.directory === path.dirname(insidePath) && event.file === 'inside.file', 'creation inside');
        await watchCountReaches(otherWatches + 4);
      }, [path.join(watchedPath, 'excluded')]);
    });
  }

  // archive's scan sees moving before the moves' events are read, and the moves say it left for an excluded path, so
  // it's watched where that scan saw it: like a directory that moves in for the first time, without what its old
  // watch queued while it was excluded.
  it('does not report what a directory queued while excluded once a scan saw it where it moved from there',
    async function() {
      await mkdirp(path.join(watchedPath, 'moving', 'inner'));
      touch(path.join('moving', 'inner', 'existing.file'));

      await watchWhile(async events => {
        await whileHeld(async () => {
          await mkdirp(path.join(watchedPath, 'archive'));
          await fsp.rename(path.join(watchedPath, 'moving'), path.join(watchedPath, 'excluded'));
          await remove(path.join(watchedPath, 'excluded', 'inner', 'existing.file'));
          await fsp.rename(path.join(watchedPath, 'excluded'), path.join(watchedPath, 'archive', 'moving'));
        });
        await barrier(events);
        await expectWatched(events, ['archive', 'moving', 'inner']);
        assert.deepStrictEqual(events.filter(event => event.file === 'existing.file'), []);
      }, [path.join(watchedPath, 'excluded')]);
    });

  it('watches a directory moved into a directory created in the same read, and the one taking its place there',
    async function() {
      await mkdirp(path.join(watchedPath, 'moving', 'inner'));
      const otherWatches = countInotifyWatches();

      await watchWhile(async events => {
        await whileHeld(async () => {
          await mkdirp(path.join(watchedPath, 'archive'));
          await fsp.rename(path.join(watchedPath, 'moving'), path.join(watchedPath, 'archive', 'moving'));
          await fsp.rename(path.join(watchedPath, 'archive', 'moving'), path.join(watchedPath, 'archive', 'moved'));
          await mkdirp(path.join(watchedPath, 'archive', 'moving'));
        });
        for (const directory of [['moved', 'inner'], ['moving']]) {
          const insidePath = path.join(watchedPath, 'archive', ...directory, 'inside.file');
          await fsp.writeFile(insidePath, 'Inside');
          await waitFor(events, event => event.action === nsfw.actions.CREATED &&
            event.directory === path.dirname(insidePath) && event.file === 'inside.file', 'creation inside');
        }
        // The watched path, archive, moved, inner and the new moving.
        await watchCountReaches(otherWatches + 5);
      });
    });

  it('watches a directory moved into a directory that moves on in the same read', async function() {
    await mkdirp(path.join(watchedPath, 'moving', 'inner'));

    await watchWhile(async events => {
      await whileHeld(async () => {
        await mkdirp(path.join(watchedPath, 'archive'));
        await fsp.rename(path.join(watchedPath, 'moving'), path.join(watchedPath, 'archive', 'moving'));
        await fsp.rename(path.join(watchedPath, 'archive'), path.join(watchedPath, 'archived'));
      });
      const insidePath = path.join(watchedPath, 'archived', 'moving', 'inner', 'inside.file');
      await fsp.writeFile(insidePath, 'Inside');
      await waitFor(events, event => event.action === nsfw.actions.CREATED &&
        event.directory === path.dirname(insidePath) && event.file === 'inside.file', 'creation inside');
    });
  });

  // The kernel queues the same events when moving left the watched path, and was moved into archived's new place
  // from there, with the deletion made outside: see 'does not report what a directory moved out ... queued while it
  // couldn't be watched'. So moving is watched like a directory that moves in for the first time, and the deletion
  // right after may have no event of its own. Whether it has one isn't checked, so that a way to tell the two apart
  // doesn't break this; the specs of that other case check that none of what its old watch queued before it was
  // found again is reported.
  it('watches a directory moved into a directory that moves on in the same read like one that moves in for the ' +
    'first time', async function() {
    await mkdirp(path.join(watchedPath, 'moving', 'inner'));
    touch(path.join('moving', 'inner', 'existing.file'));
    touch(path.join('moving', 'inner', 'other.file'));

    await watchWhile(async events => {
      await whileHeld(async () => {
        await mkdirp(path.join(watchedPath, 'archive'));
        await fsp.rename(path.join(watchedPath, 'moving'), path.join(watchedPath, 'archive', 'moving'));
        await fsp.rename(path.join(watchedPath, 'archive'), path.join(watchedPath, 'archived'));
      });
      const innerPath = path.join(watchedPath, 'archived', 'moving', 'inner');
      await remove(path.join(innerPath, 'existing.file'));
      await barrier(events);
      await remove(path.join(innerPath, 'other.file'));
      await waitFor(events, event => event.action === nsfw.actions.DELETED && event.directory === innerPath &&
        event.file === 'other.file', 'deletion inside');

      // What was reported adds up to what is there: no creation of the deleted file left standing.
      const existing = events.filter(event => event.file === 'existing.file').map(event => event.action);
      assert.ok(existing.lastIndexOf(nsfw.actions.CREATED) < existing.lastIndexOf(nsfw.actions.DELETED) ||
        !existing.includes(nsfw.actions.CREATED), JSON.stringify(existing));
      assert.deepStrictEqual(
        events.filter(event => event.directory.startsWith(path.join(watchedPath, 'moving'))), []);
    });
  });

  it('watches a directory moved out and back in within one read after a deletion', async function() {
    await mkdirp(path.join(watchedPath, 'round', 'trip'));
    touch(path.join('round', 'trip', 'existing.file'));

    await watchWhile(async events => {
      await whileHeld(async () => {
        await fsp.rename(path.join(watchedPath, 'round'), path.join(outsidePath, 'round'));
        await fsp.rename(path.join(outsidePath, 'round'), path.join(watchedPath, 'round'));
      });
      const existingPath = path.join(watchedPath, 'round', 'trip', 'existing.file');
      await remove(existingPath);
      await checkReturnState(events, path.dirname(existingPath), 'existing.file', false, true);
    });
  });

  // Nothing orders the arrival of s2 below e against what its watch queued while excluded, whether that arrival is
  // read with e's return or in a later read. inotify pads names this short to 32-byte events, so 300 creations take
  // more than a read (8 KB): the deletion and the move below e then come in a read after the one e's return is in.
  for (const [description, fillers] of [['', 0], [', in a later read', 300]]) {
    it('does not report what an excluded directory queued once it moved into a directory that came back before it' +
      description, async function() {
      await mkdirp(path.join(watchedPath, 'd'));
      await mkdirp(path.join(watchedPath, 's2'));
      await mkdirp(path.join(watchedPath, 'x'));
      fs.writeFileSync(path.join(watchedPath, 's2', 'secret.file'), 'Secret');

      await watchWhile(async events => {
        await whileHeld(async () => {
          fs.renameSync(path.join(watchedPath, 'd'), path.join(watchedPath, 'x', 'd'));
          fs.renameSync(path.join(watchedPath, 's2'), path.join(watchedPath, 'x', 's2'));
          fs.renameSync(path.join(watchedPath, 'x', 'd'), path.join(watchedPath, 'e'));
          for (let i = 0; i < fillers; ++i) {
            touch(`f${i}`);
          }
          fs.unlinkSync(path.join(watchedPath, 'x', 's2', 'secret.file'));
          fs.renameSync(path.join(watchedPath, 'x', 's2'), path.join(watchedPath, 'e', 's2'));
        });
        await barrier(events);
        await fsp.writeFile(path.join(watchedPath, 'e', 's2', 'top.file'), 'Top');
        await waitFor(events, event => event.action === nsfw.actions.CREATED &&
          event.directory === path.join(watchedPath, 'e', 's2') && event.file === 'top.file', 'creation in s2');
        assert.deepStrictEqual(events.filter(event => event.file === 'secret.file'), []);
      }, [path.join(watchedPath, 'x')]);
    });
  }

  it('does not report what an excluded directory queued once it moved into a directory created in the same read',
    async function() {
      await mkdirp(path.join(watchedPath, 'd', 'sub'));
      await mkdirp(path.join(watchedPath, 'x'));
      fs.writeFileSync(path.join(watchedPath, 'd', 'sub', 'secret.file'), 'Secret');

      await watchWhile(async events => {
        await whileHeld(async () => {
          fs.renameSync(path.join(watchedPath, 'd'), path.join(watchedPath, 'x', 'd'));
          // archive isn't watched yet when d moves in, so no event tells when d arrived there.
          fs.mkdirSync(path.join(watchedPath, 'archive'));
          fs.unlinkSync(path.join(watchedPath, 'x', 'd', 'sub', 'secret.file'));
          fs.renameSync(path.join(watchedPath, 'x', 'd'), path.join(watchedPath, 'archive', 'd'));
        });
        await barrier(events);
        await fsp.writeFile(path.join(watchedPath, 'archive', 'd', 'sub', 'top.file'), 'Top');
        const created = event => event.action === nsfw.actions.CREATED &&
          event.directory === path.join(watchedPath, 'archive', 'd', 'sub') && event.file === 'top.file';
        await waitFor(events, created, 'creation in sub');
        assert.deepStrictEqual(events.filter(event => event.file === 'secret.file'), []);
      }, [path.join(watchedPath, 'x')]);
    });

  it('does not report what an excluded directory queued once updateExcludedPaths() includes it again',
    async function() {
      await mkdirp(path.join(watchedPath, 'd', 'sub'));
      const excludedPath = path.join(watchedPath, 'd');

      await watchWhile(async (events, watch) => {
        // The loop waits after reading the pause file, without its lock.
        await untilLoopWaits('nsfw-test-pause-before-lock', 'nsfw-test-resume-event-loop');
        // Still queued when d is excluded, so its watches aren't removed yet.
        fs.writeFileSync(path.join(watchedPath, 'd', 'sub', 'before.file'), 'Before');
        await watch.updateExcludedPaths([excludedPath]);
        fs.writeFileSync(path.join(watchedPath, 'd', 'sub', 'secret.file'), 'Secret');
        await watch.updateExcludedPaths([]);
        touch('nsfw-test-resume-event-loop');

        await barrier(events);
        await fsp.writeFile(path.join(watchedPath, 'd', 'sub', 'top.file'), 'Top');
        const created = event => event.action === nsfw.actions.CREATED &&
          event.directory === path.join(watchedPath, 'd', 'sub') && event.file === 'top.file';
        await waitFor(events, created, 'creation in sub');
        assert.deepStrictEqual(events.filter(event => event.file === 'secret.file'), []);
      });
    });

  it('does not report what an excluded directory queued once included again twice before the loop reads',
    async function() {
      await mkdirp(path.join(watchedPath, 'd', 'sub'));
      const excludedPath = path.join(watchedPath, 'd');

      await watchWhile(async (events, watch) => {
        // The loop waits after reading the pause file, without its lock.
        await untilLoopWaits('nsfw-test-pause-before-lock', 'nsfw-test-resume-event-loop');
        fs.writeFileSync(path.join(watchedPath, 'd', 'sub', 'before.file'), 'Before');
        // The second inclusion's marker comes after the first one's: neither lifts what the other quarantines early.
        for (const file of ['secret.file', 'secret2.file']) {
          await watch.updateExcludedPaths([excludedPath]);
          fs.writeFileSync(path.join(watchedPath, 'd', 'sub', file), 'Secret');
          await watch.updateExcludedPaths([]);
        }
        touch('nsfw-test-resume-event-loop');

        await barrier(events);
        await fsp.writeFile(path.join(watchedPath, 'd', 'sub', 'top.file'), 'Top');
        const created = event => event.action === nsfw.actions.CREATED &&
          event.directory === path.join(watchedPath, 'd', 'sub') && event.file === 'top.file';
        await waitFor(events, created, 'creation in sub');
        assert.deepStrictEqual(events.filter(event => (event.file || '').startsWith('secret')), []);
      });
    });
});
