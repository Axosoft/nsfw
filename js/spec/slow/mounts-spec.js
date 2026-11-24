const { describe } = require('node:test');
const assert = require('assert');
const cp = require('child_process');
const fs = require('fs');
const fsp = require('fs/promises');
const path = require('path');
const nsfw = require('../../dist');
const { it, mkdirp, sleep } = require('../common');
const fixture = require('./fixture');

describe('inotify mounts (slow)', { skip: process.platform !== 'linux' }, function() {
  const {
    watchedPath, outsidePath, touch, watchWhile, waitFor,
    whileHeld, barrier, reportedIn, withMount, expectWatched
  } = fixture();

  it('watches btrfs subvolumes with the same inode numbers', async function(t) {
    // Some 32-bit platforms sign-extend the filesystem magic.
    const btrfsMagic = global.BigInt(0x9123683e);
    if (!fs.statfsSync ||
      global.BigInt.asUintN(32, fs.statfsSync(watchedPath, { bigint: true }).type) !== btrfsMagic ||
      cp.spawnSync('btrfs', ['version'], { stdio: 'ignore' }).error) {
      return t.skip();
    }

    const first = path.join(watchedPath, 'sv1');
    const second = path.join(watchedPath, 'sv2');
    for (const subvolume of [first, second]) {
      cp.execFileSync('btrfs', ['subvolume', 'create', subvolume]);
      await fsp.mkdir(path.join(subvolume, 'd'));
    }

    for (const name of ['.', 'd']) {
      const a = fs.statSync(path.join(first, name));
      const b = fs.statSync(path.join(second, name));
      assert.strictEqual(a.ino, b.ino);
      assert.notStrictEqual(a.dev, b.dev);
    }
    await fsp.symlink(watchedPath, path.join(first, 'd', 'loop'));

    await watchWhile(async events => {
      await expectWatched(events, ['sv1'], ['sv1', 'd'], ['sv2'], ['sv2', 'd']);
    });
  });

  // Mounts need privileges: as root, outside a container.
  function canMount() {
    const probe = path.join(require('os').tmpdir(), `nsfw-mount-probe-${process.pid}`);
    fs.mkdirSync(probe, { recursive: true });
    let works = false;
    try {
      cp.execFileSync('mount', ['-t', 'tmpfs', 'tmpfs', probe], { stdio: 'ignore' });
      works = true;
      cp.execFileSync('umount', ['-l', probe], { stdio: 'ignore' });
    } catch (error) {
      // No privileges.
    }
    try {
      fs.rmdirSync(probe);
    } catch (error) {
      // Left behind if it couldn't be unmounted.
    }
    return works;
  }

  describe('mounts', { skip: !canMount() }, function() {
    it('watches a directory that left at a bind mount of it made in the same read, with the exclusions there',
      async function() {
        await mkdirp(path.join(watchedPath, 'z', 'sub', 'inner'));
        const aliasPath = path.join(watchedPath, 'n', 'alias');

        await watchWhile(async events => {
          await whileHeld(async () => {
            await withMount(this, ['--bind', path.join(watchedPath, 'z', 'sub')], aliasPath, async () => {
              await fsp.rename(path.join(watchedPath, 'z', 'sub'), path.join(outsidePath, 'sub'));
              touch('nsfw-test-release-event-loop');
              assert.deepStrictEqual(
                await reportedIn(events, path.join(outsidePath, 'sub'), ['top.file', 'inner/deep.file']),
                [path.join('n', 'alias', 'top.file')]
              );
            });
          });
        }, [path.join(aliasPath, 'inner')]);
      });

    it('watches what a directory excluded by a move holds, once a bind mount of it takes it where it is not excluded',
      async function() {
        await mkdirp(path.join(watchedPath, 'old', 'c', 'e1', 'e2'));
        await mkdirp(path.join(watchedPath, 'new'));
        const aliasPath = path.join(watchedPath, 'n', 'alias');

        await watchWhile(async events => {
          await whileHeld(async () => {
            await withMount(this, ['--bind', path.join(watchedPath, 'old', 'c', 'e1')], aliasPath, async () => {
              touch('nsfw-test-release-event-loop');
              await barrier(events);
              // e1 and e2 are excluded where c goes, so e1 goes to the bind mount, the one place it can be watched,
              // where e2 isn't excluded.
              await fsp.rename(path.join(watchedPath, 'old', 'c'), path.join(watchedPath, 'new', 'c'));
              assert.deepStrictEqual(
                await reportedIn(events, aliasPath, ['top.file', 'e2/deep.file']),
                [path.join('n', 'alias', 'top.file'), path.join('n', 'alias', 'e2', 'deep.file')]
              );
            });
          });
        }, [path.join(watchedPath, 'new', 'c', 'e1'), path.join(watchedPath, 'new', 'c', 'e1', 'e2')]);
      });

    it('does not watch a directory that leaves after a bind mount of it seen by a scan is gone', async function() {
      await mkdirp(path.join(watchedPath, 'z', 'sub', 'inner'));
      const aliasPath = path.join(watchedPath, 'n', 'alias');

      await watchWhile(async events => {
        await whileHeld(async () => {
          // Gone once its scan is handled.
          await withMount(this, ['--bind', path.join(watchedPath, 'z', 'sub')], aliasPath, async () => {
            touch('nsfw-test-release-event-loop');
            await barrier(events);
          });
        });
        await fsp.rename(path.join(watchedPath, 'z', 'sub'), path.join(outsidePath, 'sub'));
        assert.deepStrictEqual(
          await reportedIn(events, path.join(outsidePath, 'sub'), ['top.file', 'inner/deep.file']), []);
      });
    });

    it('keeps the exclusions of a directory found through a bind mount while its parent moves', async function() {
      await mkdirp(path.join(watchedPath, 'parent', 'real', 'sub'));
      const aliasPath = path.join(watchedPath, 'aliases', 'alias');
      const excludedPath = path.join(watchedPath, 'moved', 'real', 'sub');

      await watchWhile(async events => {
        await whileHeld(async () => {
          await withMount(this, ['--bind', path.join(watchedPath, 'parent', 'real')], aliasPath, async () => {
            await fsp.rename(path.join(watchedPath, 'parent'), path.join(watchedPath, 'moved'));
            touch('nsfw-test-release-event-loop');
            await barrier(events);
            await fsp.writeFile(path.join(excludedPath, 'excluded.file'), 'Excluded');
            await sleep(500);
            await barrier(events);
            assert.deepStrictEqual(events.filter(event => event.file === 'excluded.file'), []);
          });
        });
      }, [excludedPath]);
    });

    // As the symlink one above, with a bind mount of the directory, which a scan sees as a directory of its own.
    it('watches a directory a scan saw at a bind mount of it at its real path, once its move is handled',
      async function() {
        await mkdirp(path.join(watchedPath, 'a'));
        await mkdirp(path.join(outsidePath, 'P', 'z', 'm'));

        await watchWhile(async events => {
          await whileHeld(async () => {
            fs.renameSync(path.join(watchedPath, 'a'), path.join(outsidePath, 'P', 'a'));
            fs.renameSync(path.join(outsidePath, 'P'), path.join(watchedPath, 'P'));
          });
          await waitFor(events, event => event.action === nsfw.actions.CREATED &&
            event.directory === path.join(watchedPath, 'P') && event.file === 'z', 'scan of P');
          fs.renameSync(path.join(watchedPath, 'P', 'a'), path.join(watchedPath, 'M'));
          const mountPoint = path.join(watchedPath, 'P', 'z', 'm');
          await withMount(this, ['--bind', path.join(watchedPath, 'M')], mountPoint, async () => {
            await barrier(events);
            await expectWatched(events, ['M']);
            assert.deepStrictEqual(events.filter(event => event.directory.startsWith(mountPoint)), []);
          });
        });
      });

    // The marker of a directory found again with no other dropped watch left is its own watch's removal: it's watched
    // again at its path, and where a scan saw it meanwhile (a bind mount of it) goes with its new watch, so that
    // once it leaves, it's watched there.
    it('watches a directory at a bind mount of it a scan saw while it was found again, once it leaves',
      async function() {
        await mkdirp(path.join(watchedPath, 'a'));
        await mkdirp(path.join(outsidePath, 'P', 'z', 'm'));

        await watchWhile(async events => {
          await whileHeld(async () => {
            fs.renameSync(path.join(watchedPath, 'a'), path.join(outsidePath, 'P', 'a'));
            fs.renameSync(path.join(outsidePath, 'P'), path.join(watchedPath, 'P'));
          });
          await waitFor(events, event => event.action === nsfw.actions.CREATED &&
            event.directory === path.join(watchedPath, 'P') && event.file === 'z', 'scan of P');
          const mountPoint = path.join(watchedPath, 'P', 'z', 'm');
          await withMount(this, ['--bind', path.join(watchedPath, 'P', 'a')], mountPoint, async () => {
            await barrier(events);
            fs.renameSync(path.join(watchedPath, 'P', 'a'), path.join(outsidePath, 'a'));
            await barrier(events);
            await expectWatched(events, ['P', 'z', 'm']);
          });
        });
      });

    it('watches a directory moved out of the source of a bind mount scanned before it', async function() {
      await mkdirp(path.join(watchedPath, 'z-source', 'moving', 'inner'));
      await mkdirp(path.join(watchedPath, 'z-source', 'moving', 'excluded'));
      const aliasPath = path.join(watchedPath, 'a-alias');
      const excludedPath = path.join(watchedPath, 'archive', 'moving', 'excluded');

      await withMount(this, ['--bind', path.join(watchedPath, 'z-source')], aliasPath, async () => {
        await watchWhile(async events => {
          await whileHeld(async () => {
            await mkdirp(path.join(watchedPath, 'archive'));
            await fsp.rename(path.join(watchedPath, 'z-source', 'moving'),
              path.join(watchedPath, 'archive', 'moving'));
          });
          const insidePath = path.join(watchedPath, 'archive', 'moving', 'inner', 'inside.file');
          await fsp.writeFile(insidePath, 'Inside');
          await waitFor(events, event => event.action === nsfw.actions.CREATED &&
            event.directory === path.dirname(insidePath) && event.file === 'inside.file', 'creation inside');
          // With the exclusions of its new path.
          await fsp.writeFile(path.join(excludedPath, 'excluded.file'), 'Excluded');
          await barrier(events);
          assert.deepStrictEqual(events.filter(event => event.file === 'excluded.file'), []);
        }, [excludedPath]);
      });
    });

    it('watches a directory moved into a directory created in the same read, below a symlink to another mount',
      async function() {
        const mountPoint = path.join(outsidePath, 'mount');

        await withMount(this, ['-t', 'tmpfs', 'tmpfs'], mountPoint, async () => {
          await mkdirp(path.join(mountPoint, 'moving', 'inner'));
          await fsp.symlink(mountPoint, path.join(watchedPath, 'link'));

          await watchWhile(async events => {
            await whileHeld(async () => {
              await mkdirp(path.join(mountPoint, 'archive'));
              await fsp.rename(path.join(mountPoint, 'moving'), path.join(mountPoint, 'archive', 'moving'));
            });
            const insidePath = path.join(watchedPath, 'link', 'archive', 'moving', 'inner', 'inside.file');
            await fsp.writeFile(insidePath, 'Inside');
            await waitFor(events, event => event.action === nsfw.actions.CREATED &&
              event.directory === path.dirname(insidePath) && event.file === 'inside.file', 'creation inside');
          });
        });
      });
  });
});
