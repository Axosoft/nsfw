const assert = require('assert');
const fs = require('fs');
const fse = require('fs-extra');
const path = require('path');
const nsfw = require('../../src');
const { WORKDIR: workDir, sleep } = require('../common');
const fixture = require('./fixture');

describe('inotify aliases (slow)', function() {
  this.timeout(120000);
  const {
    watchedPath, outsidePath, touch, watchWhile, waitFor,
    checkReturnState, waitingPath, untilLoopWaits, whileHeld, barrier, reportedIn,
    whileScanning, expectWatched
  } = fixture();


  async function waitForRegistration(signal, attempt) {
    for (let i = 0; i < 500 && fs.statSync(signal).size < attempt; ++i) await sleep(10);
    assert.strictEqual(fs.statSync(signal).size, attempt);
  }

  it('finishes re-including a link whose external directory changes during registration', async function() {
    const link = path.join(watchedPath, 'nsfw-test-changing-link');
    const target = path.join(outsidePath, 'target');
    const signal = watchedPath + '.acquiring';
    await fse.ensureDir(watchedPath);
    await fse.ensureDir(target);
    await fse.symlink(target, link);
    await fse.writeFile(signal, '');
    await watchWhile(async (events, watch) => {
      const update = watch.updateExcludedPaths([]);
      for (let attempt = 1; attempt <= 2; ++attempt) {
        await waitForRegistration(signal, attempt);
        fs.renameSync(target, target + attempt);
        fs.mkdirSync(target);
      }
      await update;
      await fse.writeFile(path.join(target, 'after-update'), '');
      await waitFor(events, event => event.action === nsfw.actions.CREATED && event.directory === link &&
        event.file === 'after-update', 'creation in replacement target after update');
    }, [link]);
  });

  it('keeps an owned watch and acquires the external target visible after registration', async function() {
    const link = path.join(watchedPath, 'nsfw-test-changing-link');
    const owned = path.join(watchedPath, 'owned');
    const target = path.join(outsidePath, 'target');
    await fse.ensureDir(owned);
    await fse.ensureDir(target);
    await fse.symlink(target, link);
    const acquiring = watchedPath + '.acquiring';
    const registered = watchedPath + '.registered';
    await fse.writeFile(acquiring, '');
    await fse.writeFile(registered, '');
    await watchWhile(async (events, watch) => {
      const update = watch.updateExcludedPaths([]);
      await waitForRegistration(acquiring, 1);
      fs.renameSync(target, target + '-first');
      fs.symlinkSync(owned, target);
      await waitForRegistration(registered, 1);
      fs.unlinkSync(target);
      fs.mkdirSync(target);
      await update;
      for (const directory of [owned, link]) {
        await fse.writeFile(path.join(directory, 'after-update'), '');
        await waitFor(events, event => event.action === nsfw.actions.CREATED && event.directory === directory &&
          event.file === 'after-update', 'creation in ' + directory);
      }
    }, [link]);
  });

  it('watches a directory that left at a symlink to it created in the same read, with the exclusions there',
    async function() {
      await fse.ensureDir(path.join(watchedPath, 'real', 'inner'));
      const linkPath = path.join(watchedPath, 'links', 'link');

      await watchWhile(async events => {
        await whileHeld(async () => {
          await fse.ensureDir(path.join(watchedPath, 'links'));
          await fse.rename(path.join(watchedPath, 'real'), path.join(outsidePath, 'moved'));
          await fse.symlink(path.join(outsidePath, 'moved'), linkPath);
        });
        assert.deepStrictEqual(
          await reportedIn(events, path.join(outsidePath, 'moved'), ['top.file', 'inner/deep.file']),
          [path.join('links', 'link', 'top.file')]
        );
      }, [path.join(linkPath, 'inner')]);
    });

  describe('a directory watched at a symlink scanned before its own path', function() {
    // What a read holds of the directory when its node takes its own path: nothing of it is dropped, and nothing it
    // holds is reported as created.
    async function expectReportedAtOwnPath(events, ownPath, linkPath) {
      await barrier(events);
      const reported = events.filter(event => ['held.file', 'written.file'].includes(event.file))
        .filter(event => event.action !== nsfw.actions.MODIFIED)
        .map(event => [event.action, path.join(event.directory, event.file)]);
      assert.deepStrictEqual(reported, [
        [nsfw.actions.DELETED, path.join(ownPath, 'sub', 'held.file')],
        [nsfw.actions.CREATED, path.join(ownPath, 'written.file')]
      ]);
      assert.deepStrictEqual(events.filter(event => event.directory.startsWith(linkPath)), []);
    }

    it('reports at its own path what was queued for it when an update excluded the symlink', async function() {
      const holderPath = path.join(watchedPath, 'node_modules');
      const ownPath = path.join(watchedPath, 'packages', 'pkg');
      await fse.outputFile(path.join(ownPath, 'sub', 'held.file'), 'Held');
      await fse.ensureDir(holderPath);
      await fse.symlink(path.join('..', 'packages', 'pkg'), path.join(holderPath, 'pkg'));

      await watchWhile(async (events, watch) => {
        // The read is held without the lock, so the update goes first.
        await untilLoopWaits('nsfw-test-pause-before-lock', 'nsfw-test-resume-event-loop');
        await fse.remove(path.join(ownPath, 'sub', 'held.file'));
        await fse.writeFile(path.join(ownPath, 'written.file'), 'Written');
        await watch.updateExcludedPaths([holderPath]);
        touch('nsfw-test-resume-event-loop');
        await expectReportedAtOwnPath(events, ownPath, holderPath);
      });
    });

    it('reports at its own path what the read of a rename that left the symlink dangling holds', async function() {
      const linkPath = path.join(watchedPath, 'a-link');
      const ownPath = path.join(watchedPath, 'moved', 'real');
      await fse.outputFile(path.join(watchedPath, 'deep', 'real', 'sub', 'held.file'), 'Held');
      await fse.symlink(path.join('deep', 'real'), linkPath);

      await watchWhile(async events => {
        await whileHeld(async () => {
          await fse.rename(path.join(watchedPath, 'deep'), path.join(watchedPath, 'moved'));
          await fse.remove(path.join(ownPath, 'sub', 'held.file'));
          await fse.writeFile(path.join(ownPath, 'written.file'), 'Written');
        });
        await expectReportedAtOwnPath(events, ownPath, linkPath);
      });
    });
  });

  for (const [description, operation] of [
    ['before', async () => {
      await fse.rename(path.join(watchedPath, 'p', 'X'), path.join(watchedPath, 'A', 'X'));
      await fse.rename(path.join(watchedPath, 'p'), path.join(outsidePath, 'p'));
    }],
    ['after', async () => {
      await fse.rename(path.join(watchedPath, 'p'), path.join(outsidePath, 'p'));
      await fse.rename(path.join(outsidePath, 'p', 'X'), path.join(watchedPath, 'A', 'X'));
    }]
  ]) {
    it(`watches a directory moved into a directory created in the same read, ${description} its old parent leaves`,
      async function() {
        await fse.ensureDir(path.join(watchedPath, 'p', 'X', 'inner'));

        await watchWhile(async events => {
          await whileHeld(async () => {
            await fse.ensureDir(path.join(watchedPath, 'A'));
            await operation();
          });
          assert.deepStrictEqual(
            await reportedIn(events, path.join(watchedPath, 'A', 'X'), ['top.file', 'inner/deep.file']),
            [path.join('A', 'X', 'top.file'), path.join('A', 'X', 'inner', 'deep.file')]
          );
        });
      });
  }

  // With no other dropped watch left, the marker of a directory found again is the removal of its own watch, and it's
  // watched again at its path. If it left that path meanwhile, for one a scan saw it at, its node goes: the event
  // that tells it left finds no node, and the directory is watched where the scan saw it instead.
  it('watches a directory a scan saw elsewhere, once the watch it was found again with made a marker',
    async function() {
      await fse.ensureDir(path.join(watchedPath, 'a'));
      await fse.ensureDir(path.join(outsidePath, 'P', 'z'));

      await watchWhile(async events => {
        await whileHeld(async () => {
          fs.renameSync(path.join(watchedPath, 'a'), path.join(outsidePath, 'P', 'a'));
          fs.renameSync(path.join(outsidePath, 'P'), path.join(watchedPath, 'P'));
        });
        // P's scan finds a again, with its old watch, then reports z just before it builds z's node, which the slow
        // build takes 500 ms to do before adding its watch: z isn't watched yet when a moves into it.
        await waitFor(events, event => event.action === nsfw.actions.CREATED &&
          event.directory === path.join(watchedPath, 'P') && event.file === 'z', 'scan of P');
        fs.renameSync(path.join(watchedPath, 'P', 'a'), path.join(watchedPath, 'P', 'z', 'a2'));

        await barrier(events);
        await expectWatched(events, ['P', 'z', 'a2']);
      });
    });

  // Same as above, but what takes the place of the directory found again is a symlink to the watched path: watching
  // it again there adds to the watched path's own watch, which keeps telling when the watched path moves.
  it('reports a moved root after a directory found again leaves a symlink to the root at its path',
    async function() {
      await fse.ensureDir(path.join(watchedPath, 'a'));
      await fse.ensureDir(path.join(outsidePath, 'P', 'z'));
      const events = [];
      let resolveError;
      const errored = new Promise(resolve => { resolveError = resolve; });
      const watch = await nsfw(watchedPath, batch => events.push(...batch), {
        debounceMS: 100, errorCallback: resolveError
      });
      try {
        await watch.start();
        await whileHeld(async () => {
          fs.renameSync(path.join(watchedPath, 'a'), path.join(outsidePath, 'P', 'a'));
          fs.renameSync(path.join(outsidePath, 'P'), path.join(watchedPath, 'P'));
        });
        await waitFor(events, event => event.action === nsfw.actions.CREATED &&
          event.directory === path.join(watchedPath, 'P') && event.file === 'z', 'scan of P');
        fs.renameSync(path.join(watchedPath, 'P', 'a'), path.join(outsidePath, 'a2'));
        fs.symlinkSync(watchedPath, path.join(watchedPath, 'P', 'a'));
        await barrier(events);

        // In the same tick, so the poll thread's stat() of the watched path finds the new directory: only the old
        // one's IN_MOVE_SELF tells.
        fs.renameSync(watchedPath, path.join(workDir, 'moved-root'));
        fs.mkdirSync(watchedPath);
        const error = await Promise.race([errored, sleep(5000).then(() => null)]);
        assert.ok(error instanceof Error, `errorCallback was not called; events ${JSON.stringify(events)}`);
        assert.strictEqual(error.message, 'Service shutdown: root path changed (renamed or deleted)');
      } finally {
        await watch.stop().catch(() => {});
      }
    });

  // A symlink in z leads to where a moves next, in the watched path: z's scan sees
  // a there, and a's move is still queued when the read that found it again ends.
  it('watches a directory a scan saw through a symlink at its real path, once its move is handled', async function() {
    await fse.ensureDir(path.join(watchedPath, 'a'));
    await fse.ensureDir(path.join(outsidePath, 'P', 'z'));

    await watchWhile(async events => {
      await whileHeld(async () => {
        fs.renameSync(path.join(watchedPath, 'a'), path.join(outsidePath, 'P', 'a'));
        fs.renameSync(path.join(outsidePath, 'P'), path.join(watchedPath, 'P'));
      });
      await waitFor(events, event => event.action === nsfw.actions.CREATED &&
        event.directory === path.join(watchedPath, 'P') && event.file === 'z', 'scan of P');
      fs.renameSync(path.join(watchedPath, 'P', 'a'), path.join(watchedPath, 'M'));
      fs.symlinkSync(path.join(watchedPath, 'M'), path.join(watchedPath, 'P', 'z', 'alias'));

      await barrier(events);
      await expectWatched(events, ['M']);
      const aliasPath = path.join(watchedPath, 'P', 'z', 'alias');
      assert.deepStrictEqual(events.filter(event => event.directory.startsWith(aliasPath)), []);
    });
  });

  // Here a leaves the watched path and comes back in two moves, and the marker ends a
  // read with a's way out, while its way back in is in the next one: a retry at the end of that read, with the move
  // still pending, would watch a at the sighted place before the next read gives it a node at M. The second hold, in
  // h, keeps the event loop in its read once P's pass is settled, so the way back in is queued after the marker.
  it('watches a directory a scan saw at its real path, once a move pending when its marker was read is handled',
    async function() {
      await fse.ensureDir(path.join(watchedPath, 'a'));
      await fse.ensureDir(path.join(watchedPath, 'h'));
      await fse.ensureDir(path.join(outsidePath, 'P', 'z'));
      const hop = path.join(outsidePath, 'hop');
      const aliasPath = path.join(watchedPath, 'P', 'z', 'alias');

      await watchWhile(async events => {
        await whileHeld(async () => {
          fs.renameSync(path.join(watchedPath, 'a'), path.join(outsidePath, 'P', 'a'));
          fs.renameSync(path.join(outsidePath, 'P'), path.join(watchedPath, 'P'));
          await fse.remove(waitingPath);
          fs.closeSync(fs.openSync(path.join(watchedPath, 'h', 'nsfw-test-hold-event-loop'), 'w'));
        });
        // z's node takes 500 ms before its watch and scan. inotify pads names this short to 32-byte events: 254
        // creations, a's IN_MOVED_FROM and the marker's IN_IGNORED (16 bytes) fill 8176 of the 8192 bytes a read
        // takes, so a's IN_MOVED_TO, queued after them, starts the next read.
        await waitFor(events, event => event.action === nsfw.actions.CREATED &&
          event.directory === path.join(watchedPath, 'P') && event.file === 'z', 'scan of P');
        for (let i = 0; i < 254; ++i) {
          touch(`f${i}`);
        }
        fs.renameSync(path.join(watchedPath, 'P', 'a'), path.join(outsidePath, 'a'));
        fs.symlinkSync(path.join(outsidePath, 'a'), hop);
        fs.symlinkSync(hop, aliasPath);
        for (let i = 0; i < 200 && !fs.existsSync(waitingPath); ++i) {
          await sleep(50);
        }
        assert.ok(fs.existsSync(waitingPath), 'The event loop never waited in h');
        fs.renameSync(path.join(outsidePath, 'a'), path.join(watchedPath, 'M'));
        fs.unlinkSync(hop);
        fs.symlinkSync(path.join(watchedPath, 'M'), hop);
        fs.closeSync(fs.openSync(path.join(watchedPath, 'h', 'nsfw-test-release-event-loop'), 'w'));

        await barrier(events);
        await expectWatched(events, ['M']);
        assert.deepStrictEqual(events.filter(event => event.directory.startsWith(aliasPath)), []);
      });
    });

  it('does not watch a directory seen by a scan that then moved into an excluded directory', async function() {
    await fse.ensureDir(path.join(watchedPath, 'X', 'inner'));
    await fse.ensureDir(path.join(watchedPath, 'excluded'));

    await watchWhile(async events => {
      await whileScanning(events, () => fse.rename(path.join(watchedPath, 'archive', 'x'),
        path.join(watchedPath, 'excluded', 'x')));
      assert.deepStrictEqual(
        await reportedIn(events, path.join(watchedPath, 'excluded', 'x'), ['top.file', 'inner/deep.file']), []);
    }, [path.join(watchedPath, 'excluded')]);
  });

  it('does not watch a directory seen by a scan of one that then left the watched path', async function() {
    await fse.ensureDir(path.join(watchedPath, 'X', 'inner'));

    await watchWhile(async events => {
      await whileScanning(events, () => fse.rename(path.join(watchedPath, 'archive'),
        path.join(outsidePath, 'archive')));
      assert.deepStrictEqual(
        await reportedIn(events, path.join(outsidePath, 'archive', 'x'), ['top.file', 'inner/deep.file']), []);
    });
  });

  it('watches a directory that got the inode number of one deleted in the same read', async function() {
    await fse.ensureDir(path.join(watchedPath, 'old'));
    const oldInode = fs.statSync(path.join(watchedPath, 'old')).ino;
    let reused = false;

    await watchWhile(async events => {
      await whileHeld(async () => {
        await fse.ensureDir(path.join(watchedPath, 'S'));
        fs.rmdirSync(path.join(watchedPath, 'old'));
        fs.mkdirSync(path.join(watchedPath, 'S', 'new'));
        reused = fs.statSync(path.join(watchedPath, 'S', 'new')).ino === oldInode;
      });
      if (!reused) {
        // Only what the file system chose shows the case.
        this.skip();
      }
      assert.deepStrictEqual(await reportedIn(events, path.join(watchedPath, 'S', 'new'), ['top.file']),
        [path.join('S', 'new', 'top.file')]);
    });
  });

  it('watches a directory that got the inode number of one that left and was deleted in the same read',
    async function() {
      await fse.ensureDir(path.join(watchedPath, 'old'));
      await fse.ensureDir(path.join(watchedPath, 'excluded'));
      const oldInode = fs.statSync(path.join(watchedPath, 'old')).ino;
      let reused = false;

      await watchWhile(async events => {
        await whileHeld(async () => {
          await fse.ensureDir(path.join(watchedPath, 'S'));
          fs.renameSync(path.join(watchedPath, 'old'), path.join(watchedPath, 'excluded', 'old'));
          fs.writeFileSync(path.join(watchedPath, 'excluded', 'old', 'secret.file'), 'Secret');
          fse.removeSync(path.join(watchedPath, 'excluded', 'old'));
          fs.mkdirSync(path.join(watchedPath, 'S', 'new'));
          reused = fs.statSync(path.join(watchedPath, 'S', 'new')).ino === oldInode;
        });
        if (!reused) {
          this.skip();
        }
        // S's scan saw new with old's inode number, but new isn't old: old's events are its own, not new's.
        assert.deepStrictEqual(await reportedIn(events, path.join(watchedPath, 'S', 'new'), ['top.file']),
          [path.join('S', 'new', 'top.file')]);
        assert.deepStrictEqual(events.filter(event => event.file === 'secret.file'), []);
      }, [path.join(watchedPath, 'excluded')]);
    });

  it('does not report what an excluded directory queued once a scan watches it again in the same read',
    async function() {
      await fse.ensureDir(path.join(watchedPath, 'd', 'sub'));
      await fse.ensureDir(path.join(watchedPath, 'x'));
      fs.writeFileSync(path.join(watchedPath, 'd', 'sub', 'secret.file'), 'Secret');

      await watchWhile(async events => {
        await whileHeld(async () => {
          // d leaves for the excluded x, so its watches are dropped, and are still there when e's scan finds it.
          fs.renameSync(path.join(watchedPath, 'd'), path.join(watchedPath, 'x', 'd'));
          fs.mkdirSync(path.join(watchedPath, 'e'));
          fs.rmdirSync(path.join(watchedPath, 'e'));
          fs.unlinkSync(path.join(watchedPath, 'x', 'd', 'sub', 'secret.file'));
          fs.renameSync(path.join(watchedPath, 'x', 'd'), path.join(watchedPath, 'e'));
        });
        // e/sub is only watched once its old watch is removed, and the slow build takes its time to build nodes.
        await barrier(events);
        await fse.writeFile(path.join(watchedPath, 'e', 'sub', 'top.file'), 'Top');
        await waitFor(events, event => event.action === nsfw.actions.CREATED &&
          event.directory === path.join(watchedPath, 'e', 'sub') && event.file === 'top.file', 'creation in sub');
        assert.deepStrictEqual(events.filter(event => event.file === 'secret.file'), []);
      }, [path.join(watchedPath, 'x')]);
    });

  it('does not report what an excluded directory queued once the scan of another one moved to its new place finds it',
    async function() {
      await fse.ensureDir(path.join(watchedPath, 'd'));
      await fse.ensureDir(path.join(watchedPath, 'd2', 'sub'));
      await fse.ensureDir(path.join(watchedPath, 'x'));
      fs.writeFileSync(path.join(watchedPath, 'd2', 'sub', 'secret.file'), 'Secret');

      await watchWhile(async events => {
        await whileHeld(async () => {
          fs.renameSync(path.join(watchedPath, 'd'), path.join(watchedPath, 'x', 'd'));
          fs.renameSync(path.join(watchedPath, 'd2'), path.join(watchedPath, 'x', 'd2'));
          // The scan for d's arrival at e finds d2 there, before the deletion from while d2 was excluded is read.
          fs.renameSync(path.join(watchedPath, 'x', 'd'), path.join(watchedPath, 'e'));
          fs.unlinkSync(path.join(watchedPath, 'x', 'd2', 'sub', 'secret.file'));
          fs.renameSync(path.join(watchedPath, 'e'), path.join(outsidePath, 'e'));
          fs.renameSync(path.join(watchedPath, 'x', 'd2'), path.join(watchedPath, 'e'));
        });
        // e/sub is only watched once its old watch is removed, and the slow build takes its time to build nodes.
        await barrier(events);
        await fse.writeFile(path.join(watchedPath, 'e', 'sub', 'top.file'), 'Top');
        await waitFor(events, event => event.action === nsfw.actions.CREATED &&
          event.directory === path.join(watchedPath, 'e', 'sub') && event.file === 'top.file', 'creation in sub');
        assert.deepStrictEqual(events.filter(event => event.file === 'secret.file'), []);
      }, [path.join(watchedPath, 'x')]);
    });

  it('watches a directory moved out and back after a modification',
    async function() {
      await fse.ensureDir(path.join(watchedPath, 'round', 'trip'));
      touch(path.join('round', 'trip', 'existing.file'));

      await watchWhile(async events => {
        await whileHeld(async () => {
          await fse.rename(path.join(watchedPath, 'round'), path.join(outsidePath, 'round'));
          await fse.rename(path.join(outsidePath, 'round'), path.join(watchedPath, 'round'));
        });
        const existingPath = path.join(watchedPath, 'round', 'trip', 'existing.file');
        await fse.appendFile(existingPath, 'Modified');
        await checkReturnState(events, path.dirname(existingPath), 'existing.file', true);
      });
    });

  it('does not watch a directory that came back to a parent that left in the same read', async function() {
    await fse.ensureDir(path.join(watchedPath, 'p', 'd'));

    await watchWhile(async events => {
      await whileHeld(async () => {
        await fse.rename(path.join(watchedPath, 'p', 'd'), path.join(outsidePath, 'd'));
        await fse.rename(path.join(outsidePath, 'd'), path.join(watchedPath, 'p', 'd'));
        await fse.rename(path.join(watchedPath, 'p'), path.join(outsidePath, 'p'));
      });
      await barrier(events);
      await fse.writeFile(path.join(outsidePath, 'p', 'd', 'outside.file'), 'Outside');
      await barrier(events);
      assert.deepStrictEqual(events.filter(event => event.file === 'outside.file'), []);
    });
  });

  it('watches by their events a directory that got the inode number of one deleted and the one taking its name',
    async function() {
      await fse.ensureDir(path.join(watchedPath, 'old'));
      const oldInode = fs.statSync(path.join(watchedPath, 'old')).ino;
      let reused = false;

      await watchWhile(async events => {
        await whileHeld(async () => {
          await fse.ensureDir(path.join(watchedPath, 'S'));
          fs.rmdirSync(path.join(watchedPath, 'old'));
          fs.mkdirSync(path.join(watchedPath, 'S', 'new'));
          reused = fs.statSync(path.join(watchedPath, 'S', 'new')).ino === oldInode;
          // Listed after new, so S's scan takes the slow build's 500 ms for each of them once it saw new.
          for (const name of ['z1', 'z2', 'z3']) {
            fs.mkdirSync(path.join(watchedPath, 'S', name));
          }
        });
        if (!reused) {
          this.skip();
        }
        // Before the deletion of old is handled: new leaves its name to another directory.
        await waitFor(events, event => event.action === nsfw.actions.CREATED &&
          event.directory === path.join(watchedPath, 'S') && event.file === 'new', 'scan of S');
        await fse.rename(path.join(watchedPath, 'S', 'new'), path.join(watchedPath, 'S', 'moved'));
        await fse.ensureDir(path.join(watchedPath, 'S', 'new'));

        assert.deepStrictEqual(await reportedIn(events, path.join(watchedPath, 'S', 'new'), ['new.file']),
          [path.join('S', 'new', 'new.file')]);
        assert.deepStrictEqual(await reportedIn(events, path.join(watchedPath, 'S', 'moved'), ['moved.file']),
          [path.join('S', 'moved', 'moved.file')]);
      });
    });
});
