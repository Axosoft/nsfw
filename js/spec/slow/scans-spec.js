const assert = require('assert');
const fs = require('fs');
const fse = require('fs-extra');
const path = require('path');
const nsfw = require('../../dist');
const { sleep } = require('../common');
const fixture = require('./fixture');

describe('inotify scans (slow)', function() {
  this.timeout(120000);
  const {
    watchedPath, outsidePath, touch, watchWhile, waitFor,
    waitingPath, untilLoopWaits, whileHeld, barrier, expectWatched
  } = fixture();


  it('watches a directory a move includes again once a later move in the same read takes its parent on',
    async function() {
      await fse.ensureDir(path.join(watchedPath, 'A', 'child'));

      await watchWhile(async events => {
        await whileHeld(async () => {
          // When the first move is handled, B, where child is included again, is gone already.
          await fse.rename(path.join(watchedPath, 'A'), path.join(watchedPath, 'B'));
          await fse.rename(path.join(watchedPath, 'B'), path.join(watchedPath, 'C'));
        });
        await barrier(events);
        await expectWatched(events, ['C', 'child']);
      }, [path.join(watchedPath, 'A', 'child')]);
    });

  it('reports what was created in a directory a move includes again once a later move in the same read took it on',
    async function() {
      await fse.ensureDir(path.join(watchedPath, 'source', 'blocked'));
      const includedPath = path.join(watchedPath, 'moved', 'blocked');

      await watchWhile(async events => {
        await whileHeld(async () => {
          // The first move excludes blocked and drops its watch, which has this creation queued.
          await fse.rename(path.join(watchedPath, 'source'), path.join(watchedPath, 'target'));
          await fse.rename(path.join(watchedPath, 'target'), path.join(watchedPath, 'moved'));
          await fse.writeFile(path.join(includedPath, 'inside.file'), 'Inside');
        });
        await waitFor(events, event => event.action === nsfw.actions.CREATED && event.directory === includedPath &&
          event.file === 'inside.file', 'creation in the directory included again');
      }, [path.join(watchedPath, 'target', 'blocked')]);
    });

  it('does not watch the subtree of a directory that took the place where a move includes a directory again',
    async function() {
      await fse.ensureDir(path.join(watchedPath, 'A', 'child'));
      await fse.ensureDir(path.join(outsidePath, 'X', 'child'));

      await watchWhile(async events => {
        await whileHeld(async () => {
          await fse.rename(path.join(watchedPath, 'A'), path.join(watchedPath, 'B'));
          await fse.rename(path.join(watchedPath, 'B'), path.join(watchedPath, 'C'));
          // When the first move is handled, B/child is X's.
          await fse.rename(path.join(outsidePath, 'X'), path.join(watchedPath, 'B'));
        });
        await barrier(events);
        await expectWatched(events, ['C', 'child'], ['B', 'child']);
        assert.deepStrictEqual(events.filter(event => event.directory === path.join(watchedPath, 'C', 'child') &&
          event.file.startsWith('in-B-')), []);
      }, [path.join(watchedPath, 'A', 'child')]);
    });

  it('does not watch the subtree of a directory that took the old place of a parent updateExcludedPaths() includes',
    async function() {
      await fse.ensureDir(path.join(watchedPath, 'O', 'child'));
      await fse.ensureDir(path.join(outsidePath, 'Y', 'child'));

      await watchWhile(async (events, watch) => {
        // The loop waits after reading the pause file, without its lock.
        await untilLoopWaits('nsfw-test-pause-before-lock', 'nsfw-test-resume-event-loop');
        // On disk P is O's directory, whose node's path is still O, which Y holds by then.
        await fse.rename(path.join(watchedPath, 'O'), path.join(watchedPath, 'P'));
        await fse.rename(path.join(outsidePath, 'Y'), path.join(watchedPath, 'O'));
        await watch.updateExcludedPaths([]);
        touch('nsfw-test-resume-event-loop');

        await barrier(events);
        await expectWatched(events, ['P', 'child'], ['O', 'child']);
        assert.deepStrictEqual(events.filter(event => event.directory === path.join(watchedPath, 'P', 'child') &&
          event.file.startsWith('in-O-')), []);
      }, [path.join(watchedPath, 'P', 'child')]);
    });

  for (const [description, before, after] of [
    ['its parent', ['P'], ['Q']],
    ['an ancestor of its parent', ['A', 'P'], ['B', 'P']]
  ]) {
    it(`watches a directory updateExcludedPaths() includes again once a rename of ${description} still queued then ` +
      'is handled', async function() {
      await fse.ensureDir(path.join(watchedPath, ...before, 'child'));

      await watchWhile(async (events, watch) => {
        await untilLoopWaits('nsfw-test-pause-before-lock', 'nsfw-test-resume-event-loop');
        // The tree still has the parent at its old path, where nothing is on disk by then.
        await fse.rename(path.join(watchedPath, before[0]), path.join(watchedPath, after[0]));
        await watch.updateExcludedPaths([]);
        touch('nsfw-test-resume-event-loop');

        await barrier(events);
        await expectWatched(events, [...after, 'child']);
      }, [path.join(watchedPath, ...before, 'child')]);
    });
  }

  it('watches a directory updateExcludedPaths() includes again under a parent whose old place another took',
    async function() {
      await fse.ensureDir(path.join(watchedPath, 'O', 'child'));
      await fse.ensureDir(path.join(outsidePath, 'Y', 'child'));

      await watchWhile(async (events, watch) => {
        await untilLoopWaits('nsfw-test-pause-before-lock', 'nsfw-test-resume-event-loop');
        // The tree still has O's node at O, which Y holds on disk by then.
        await fse.rename(path.join(watchedPath, 'O'), path.join(watchedPath, 'P'));
        await fse.rename(path.join(outsidePath, 'Y'), path.join(watchedPath, 'O'));
        await watch.updateExcludedPaths([]);
        touch('nsfw-test-resume-event-loop');

        await barrier(events);
        await expectWatched(events, ['P', 'child'], ['O', 'child']);
        assert.deepStrictEqual(events.filter(event => event.directory === path.join(watchedPath, 'P', 'child') &&
          event.file.startsWith('in-O-')), []);
      }, [path.join(watchedPath, 'O', 'child')]);
    });

  it('watches a directory a move includes again once its parent is renamed while its node is built',
    async function() {
      await fse.ensureDir(path.join(watchedPath, 'A', 'child'));

      await watchWhile(async events => {
        await fse.rename(path.join(watchedPath, 'A'), path.join(watchedPath, 'B'));
        // The rename is reported just before B/child's node is built, which the slow build takes 500 ms to do before
        // adding its watch, by then at a path that is gone. Reported later, B/child is watched already, and the
        // rename to C takes it along: the test passes without telling anything.
        await waitFor(events, event => event.action === nsfw.actions.RENAMED && event.newFile === 'B', 'rename');
        await fse.rename(path.join(watchedPath, 'B'), path.join(watchedPath, 'C'));

        await barrier(events);
        await expectWatched(events, ['C', 'child']);
      }, [path.join(watchedPath, 'A', 'child')]);
    });

  it('watches a directory created in one that is renamed before its creation is handled', async function() {
    await fse.ensureDir(path.join(watchedPath, 'parent'));

    await watchWhile(async events => {
      await whileHeld(async () => {
        await fse.ensureDir(path.join(watchedPath, 'parent', 'created'));
        await fse.rename(path.join(watchedPath, 'parent'), path.join(watchedPath, 'renamed'));
      });
      const insidePath = path.join(watchedPath, 'renamed', 'created', 'inside.file');
      // Until the rescan after the rename watches it.
      const reported = () => events.some(event => event.action === nsfw.actions.CREATED &&
        event.directory === path.dirname(insidePath) && event.file.startsWith('inside'));
      for (let i = 0; i < 20 && !reported(); ++i) {
        await fse.writeFile(`${insidePath}.${i}`, 'Inside');
        await sleep(500);
      }
      assert.ok(reported(), `No creation inside in ${JSON.stringify(events)}`);
    });
  });

  it('rescans a renamed directory only at its own path', async function() {
    await fse.ensureDir(path.join(watchedPath, 'A', 'm'));
    await fse.ensureDir(path.join(watchedPath, 'W', 'c'));

    await watchWhile(async events => {
      await whileHeld(async () => {
        // m's handling of the creation of x misses it, which marks m for a rescan.
        await fse.ensureDir(path.join(watchedPath, 'A', 'm', 'x'));
        await fse.rename(path.join(watchedPath, 'A', 'm', 'x'), path.join(watchedPath, 'A', 'x'));
        // When A's rename is handled, A/m on disk is W.
        await fse.rename(path.join(watchedPath, 'A'), path.join(watchedPath, 'B'));
        await fse.rename(path.join(watchedPath, 'B', 'm'), path.join(watchedPath, 'B', 'old'));
        await fse.rename(path.join(watchedPath, 'W'), path.join(watchedPath, 'B', 'm'));
      });
      await barrier(events);
      const insidePath = path.join(watchedPath, 'B', 'm', 'c', 'inside.file');
      await fse.writeFile(insidePath, 'Inside');
      await waitFor(events, event => event.action === nsfw.actions.CREATED &&
        event.directory === path.dirname(insidePath) && event.file === 'inside.file', 'creation inside');
      assert.deepStrictEqual(events.filter(event => event.directory.startsWith(path.join(watchedPath, 'B', 'old'))),
        []);
    });
  });
  it('keeps a directory where it is when a renamed one takes its parent\'s place before its creation is handled',
    async function() {
      await fse.ensureDir(path.join(watchedPath, 'A'));
      await fse.ensureDir(path.join(watchedPath, 'B', 'x'));

      await watchWhile(async events => {
        await whileHeld(async () => {
          await fse.ensureDir(path.join(watchedPath, 'A', 'x'));
          await fse.rename(path.join(watchedPath, 'A'), path.join(watchedPath, 'old'));
          await fse.rename(path.join(watchedPath, 'B'), path.join(watchedPath, 'A'));
        });
        // Written once the events are handled, so no scan finds the files, and named by directory, so a creation
        // reported under the wrong one doesn't count.
        await barrier(events);
        for (const directory of [['A', 'x'], ['old', 'x']]) {
          const insideDirectory = path.join(watchedPath, ...directory);
          const prefix = `in-${directory[0]}-`;
          const reported = () => events.some(event => event.action === nsfw.actions.CREATED &&
            event.directory === insideDirectory && event.file.startsWith(prefix));
          for (let i = 0; i < 20 && !reported(); ++i) {
            await fse.writeFile(path.join(insideDirectory, `${prefix}${i}.file`), 'Inside');
            await sleep(500);
          }
          assert.ok(reported(), `No creation in ${insideDirectory} in ${JSON.stringify(events)}`);
        }
      });
    });

  it('does not rescan, for a later rename, a directory whose earlier scan lost something', async function() {
    await fse.ensureDir(path.join(watchedPath, 'ancestor', 'node', 'child'));
    touch(path.join('ancestor', 'node', 'child', 'held.file'));

    await watchWhile(async events => {
      await whileHeld(async () => {
        // node's handling of the creation of gone misses it, which marks node for a rescan.
        await fse.ensureDir(path.join(watchedPath, 'ancestor', 'node', 'gone'));
        await fse.rename(path.join(watchedPath, 'ancestor', 'node', 'gone'),
          path.join(watchedPath, 'ancestor', 'gone'));
      });
      // Once nothing is queued, no rename can have taken gone from node anymore.
      await barrier(events);
      await whileHeld(async () => {
        await fse.rename(path.join(watchedPath, 'ancestor'), path.join(watchedPath, 'moved'));
        await fse.rename(path.join(watchedPath, 'moved', 'node', 'child'),
          path.join(watchedPath, 'moved', 'node', 'renamed'));
      });
      await waitFor(events, event => event.action === nsfw.actions.RENAMED && event.newFile === 'renamed', 'rename');
      await barrier(events);
      // A rescan of node would have taken child over and reported what it holds as created.
      assert.deepStrictEqual(events.filter(event => event.file === 'held.file'), []);
    });
  });

  it('reports the move of a directory whose old parent moves into it in the same read', async function() {
    // Excluded, so the move scans it in again at its new path, and finds its old parent there.
    const excludedPath = path.join(watchedPath, 'parent', 'moving');
    await fse.ensureDir(excludedPath);

    await watchWhile(async events => {
      await whileHeld(async () => {
        await fse.rename(path.join(watchedPath, 'parent', 'moving'), path.join(watchedPath, 'moved'));
        await fse.rename(path.join(watchedPath, 'parent'), path.join(watchedPath, 'moved', 'parent'));
      });
      await waitFor(events, event => event.action === nsfw.actions.RENAMED &&
        event.directory === path.join(watchedPath, 'parent') && event.oldFile === 'moving' &&
        event.newDirectory === watchedPath && event.newFile === 'moved', 'rename');
      const insidePath = path.join(watchedPath, 'moved', 'parent', 'inside.file');
      await fse.writeFile(insidePath, 'Inside');
      await waitFor(events, event => event.action === nsfw.actions.CREATED &&
        event.directory === path.dirname(insidePath) && event.file === 'inside.file', 'creation inside');
    }, [excludedPath]);
  });

  it('keeps watching a directory moved in the same read as a symlink to it shows up', async function() {
    await fse.ensureDir(path.join(watchedPath, 'real'));

    await watchWhile(async events => {
      await whileHeld(async () => {
        await fse.ensureDir(path.join(watchedPath, 'links'));
        await fse.rename(path.join(watchedPath, 'real'), path.join(watchedPath, 'moved'));
        await fse.symlink(path.join('..', 'moved'), path.join(watchedPath, 'links', 'link'));
      });
      const insidePath = path.join(watchedPath, 'moved', 'inside.file');
      await fse.writeFile(insidePath, 'Inside');
      await waitFor(events, event => event.action === nsfw.actions.CREATED &&
        event.directory === path.dirname(insidePath) && event.file === 'inside.file', 'creation inside');
    });
  });

  it('stops watching a directory moved into a directory that left in the same read', async function() {
    await fse.ensureDir(path.join(watchedPath, 'leaving'));
    await fse.ensureDir(path.join(watchedPath, 'following'));

    await watchWhile(async events => {
      await whileHeld(async () => {
        await fse.rename(path.join(watchedPath, 'leaving'), path.join(outsidePath, 'leaving'));
        await fse.rename(path.join(watchedPath, 'following'), path.join(outsidePath, 'leaving', 'following'));
      });
      await waitFor(events, event => event.action === nsfw.actions.DELETED && event.directory === watchedPath &&
        event.file === 'following', 'deletion of the directory that followed');
      await fse.writeFile(path.join(outsidePath, 'leaving', 'following', 'outside.file'), 'Outside');
      await barrier(events);

      assert.deepStrictEqual(events.filter(event => event.file === 'outside.file'), []);
    });
  });

  it('pairs the halves of a directory move that a read splits', async function() {
    await fse.ensureDir(path.join(watchedPath, 'moving'));

    await watchWhile(async events => {
      await whileHeld(async () => {
        // inotify pads names this short to 32-byte events, so the 256th, the move's IN_MOVED_FROM, ends the 8 KB
        // read and its IN_MOVED_TO starts the next one.
        for (let i = 0; i < 255; ++i) {
          touch(`f${i}`);
        }
        await fse.rename(path.join(watchedPath, 'moving'), path.join(watchedPath, 'moved'));
      });
      await waitFor(events, event => event.action === nsfw.actions.RENAMED &&
        event.oldFile === 'moving' && event.newFile === 'moved', 'rename');

      const insidePath = path.join(watchedPath, 'moved', 'inside.file');
      await fse.writeFile(insidePath, 'Inside');
      await waitFor(events, event => event.action === nsfw.actions.CREATED &&
        event.directory === path.dirname(insidePath) && event.file === 'inside.file', 'creation inside');
      assert.deepStrictEqual(events.filter(event => event.directory === path.join(watchedPath, 'moving')), []);
    });
  });

  it('reports a file moved in over one a scan reported while the events queued before that scan are read',
    async function() {
      await fse.ensureDir(path.join(outsidePath, 'moved'));
      await fse.writeFile(path.join(outsidePath, 'moved', 'f.file'), 'Listed');
      await fse.writeFile(path.join(outsidePath, 'g.file'), 'Moved in');
      await fse.ensureDir(path.join(watchedPath, 'held'));
      const movedPath = path.join(watchedPath, 'moved');

      await watchWhile(async events => {
        await whileHeld(async () => {
          await fse.rename(path.join(outsidePath, 'moved'), movedPath);
          await fse.remove(waitingPath);
          touch(path.join('held', 'nsfw-test-hold-event-loop'));
          // More than one read takes, 48 bytes each: the scan of moved leaves some queued, which its report outlives.
          for (let i = 0; i < 250; ++i) {
            touch(`padding-${String(i).padStart(3, '0')}.file`);
          }
        });
        for (let i = 0; i < 200 && !fs.existsSync(waitingPath); ++i) {
          await sleep(50);
        }
        assert.ok(fs.existsSync(waitingPath), 'The event loop never waited in held');
        // Read with the rest of the padding, while what the scan of moved reported still holds.
        await fse.rename(path.join(outsidePath, 'g.file'), path.join(movedPath, 'f.file'));
        touch(path.join('held', 'nsfw-test-release-event-loop'));
        await barrier(events);

        const created = events.filter(event => event.action === nsfw.actions.CREATED &&
          event.directory === movedPath && event.file === 'f.file');
        assert.strictEqual(created.length, 2, JSON.stringify(events.filter(event => event.directory === movedPath)));
      });
    });
});
