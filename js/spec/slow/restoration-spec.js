const assert = require('assert');
const fs = require('fs');
const fse = require('fs-extra');
const path = require('path');
const nsfw = require('../../dist');
const { countInotifyWatches, sleep, watchCountReaches } = require('../common');
const fixture = require('./fixture');

describe('inotify restoration (slow)', function() {
  this.timeout(120000);
  const {
    watchedPath, outsidePath, markerSignal, touch, watchWhile,
    waitFor, checkReturnState, waitingPath, untilLoopWaits, whileHeld,
    barrier, expectWatched, reportedIn
  } = fixture();

  it('restores the real entries after excluding link-held directories and keeps a linked subtree outside',
    async function() {
      const blocked = path.join(watchedPath, 'z-blocked');
      const deep = path.join(blocked, 'deep');
      const kept = path.join(watchedPath, 'zz-kept');
      await fse.ensureDir(deep);
      await fse.ensureDir(kept);
      await fse.symlink(deep, path.join(watchedPath, 'a-deep'));
      await fse.symlink(blocked, path.join(watchedPath, 'b-whole'));
      await fse.symlink(kept, path.join(blocked, 'keep-link'));

      await watchWhile(async (events, watch) => {
        assert.deepStrictEqual(await reportedIn(events, deep, ['before.file']), ['a-deep/before.file']);
        await untilLoopWaits('nsfw-test-pause-before-lock', 'nsfw-test-resume-event-loop');
        fs.writeFileSync(path.join(deep, 'hidden-queued.file'), '');
        fs.writeFileSync(path.join(kept, 'kept-queued.file'), '');
        await watch.updateExcludedPaths([blocked]);
        touch('nsfw-test-resume-event-loop');
        await barrier(events);
        assert.deepStrictEqual(events.filter(event => event.file === 'hidden-queued.file'), []);
        const keptCreations = events.filter(event => event.action === nsfw.actions.CREATED &&
          event.file === 'kept-queued.file');
        assert.deepStrictEqual(keptCreations.map(event => event.directory), [kept]);
        assert.deepStrictEqual(await reportedIn(events, blocked, ['hidden.file', 'deep/hidden-deep.file']), []);

        await watch.updateExcludedPaths([]);
        assert.deepStrictEqual(await reportedIn(events, blocked, ['restored.file', 'deep/restored-deep.file']),
          ['z-blocked/restored.file', 'z-blocked/deep/restored-deep.file']);
      });
    });

  // Resolves once a pass of directories found again had to wait for the marker of a later read.
  async function untilOneMarker() {
    for (let i = 0; i < 100 && !fs.statSync(markerSignal).size; ++i) await sleep(50);
    assert.strictEqual(fs.readFileSync(markerSignal, 'utf8'), '1');
  }

  // Each pass of directories found again gets a marker queued after its last find, and is scanned after it: what a
  // scan finds is the next pass, with a marker of its own, since the one before was queued before that find.
  describe('found again in passes', function() {
    it('does not report what a directory queued while excluded once it took the place of one a scan finds next',
      async function() {
        await fse.ensureDir(path.join(watchedPath, 'x', 'd2'));
        await fse.ensureDir(path.join(watchedPath, 'P', 'b'));
        touch(path.join('x', 'd2', 'secret.file'));

        await watchWhile(async (events, watch) => {
          await untilLoopWaits('nsfw-test-pause-before-lock', 'nsfw-test-resume-event-loop');
          // Still queued when x and P are excluded, so their watches aren't removed yet.
          touch('before.file');
          await watch.updateExcludedPaths([path.join(watchedPath, 'x'), path.join(watchedPath, 'P')]);
          // The slow build takes 500 ms to build P's node, then 500 ms more to build b's, whose watch is added at
          // about 1000 ms, after the marker of P's pass: by then b is d2, with what its watch queued while excluded.
          const update = watch.updateExcludedPaths([path.join(watchedPath, 'x')]);
          await sleep(750);
          fs.unlinkSync(path.join(watchedPath, 'x', 'd2', 'secret.file'));
          fs.rmdirSync(path.join(watchedPath, 'P', 'b'));
          fs.renameSync(path.join(watchedPath, 'x', 'd2'), path.join(watchedPath, 'P', 'b'));
          touch('nsfw-test-resume-event-loop');
          await update;

          await barrier(events);
          await expectWatched(events, ['P', 'b']);
          assert.deepStrictEqual(events.filter(event => event.file === 'secret.file'), []);
        });
      });

    it('watches a directory created in one found again after its find, before the marker of its pass',
      async function() {
        for (const name of ['b1', 'b2', 'b3']) {
          await fse.ensureDir(path.join(watchedPath, 'P', name));
        }

        await watchWhile(async (events, watch) => {
          await untilLoopWaits('nsfw-test-pause-before-lock', 'nsfw-test-resume-event-loop');
          touch('before.file');
          await watch.updateExcludedPaths([path.join(watchedPath, 'P')]);
          // The slow build takes 500 ms per node: b1 is found again at about 1000 ms, b3 at 2000, and the marker of
          // their pass comes after that. A scan of b1 when it's found would miss new.
          const update = watch.updateExcludedPaths([]);
          await sleep(1250);
          fs.mkdirSync(path.join(watchedPath, 'P', 'b1', 'new'));
          await update;
          touch('nsfw-test-resume-event-loop');

          await barrier(events);
          await expectWatched(events, ['P', 'b1', 'new']);
        });
      });
  });

  it('does not report what an excluded directory queued once it moved into one that came back with a new watch',
    async function() {
      await fse.ensureDir(path.join(watchedPath, 'd'));
      await fse.ensureDir(path.join(watchedPath, 's2'));
      await fse.ensureDir(path.join(watchedPath, 'x'));
      fs.writeFileSync(path.join(watchedPath, 's2', 'secret.file'), 'Secret');

      await watchWhile(async (events, watch) => {
        await untilLoopWaits('nsfw-test-pause-before-lock', 'nsfw-test-resume-event-loop');
        // Still queued when d is excluded, so its watch isn't removed by that call, but at its next event, read
        // before the move that brings d back: d gets a new one, while s2 still comes under the old one.
        touch('before.file');
        await watch.updateExcludedPaths([path.join(watchedPath, 'x'), path.join(watchedPath, 'd')]);
        touch(path.join('d', 'poke.file'));
        fs.renameSync(path.join(watchedPath, 's2'), path.join(watchedPath, 'x', 's2'));
        fs.renameSync(path.join(watchedPath, 'd'), path.join(watchedPath, 'e'));
        fs.unlinkSync(path.join(watchedPath, 'x', 's2', 'secret.file'));
        fs.renameSync(path.join(watchedPath, 'x', 's2'), path.join(watchedPath, 'e', 's2'));
        touch('nsfw-test-resume-event-loop');

        await barrier(events);
        await fse.writeFile(path.join(watchedPath, 'e', 's2', 'top.file'), 'Top');
        const created = event => event.action === nsfw.actions.CREATED &&
          event.directory === path.join(watchedPath, 'e', 's2') && event.file === 'top.file';
        await waitFor(events, created, 'creation in s2');
        assert.deepStrictEqual(events.filter(event => event.file === 'secret.file'), []);
      }, [path.join(watchedPath, 'x')]);
    });

  it('watches a directory moved out and back after a change outside and a deletion',
    async function() {
      await fse.ensureDir(path.join(watchedPath, 'round', 'trip'));
      touch(path.join('round', 'trip', 'existing.file'));

      await watchWhile(async events => {
        await whileHeld(async () => {
          await fse.rename(path.join(watchedPath, 'round'), path.join(outsidePath, 'round'));
          // Events on trip's watch read before the move back: the creation ends the pending move out, which drops
          // trip, and the modification comes once it is dropped. trip keeps that watch all the same.
          await fse.writeFile(path.join(outsidePath, 'round', 'trip', 'outside.file'), 'Outside');
          await fse.rename(path.join(outsidePath, 'round'), path.join(watchedPath, 'round'));
          await fse.remove(path.join(watchedPath, 'round', 'trip', 'existing.file'));
        });
        const existingPath = path.join(watchedPath, 'round', 'trip', 'existing.file');
        await checkReturnState(events, path.dirname(existingPath), 'existing.file', false, true);
      });
    });

  it('does not report what a directory moved into an excluded one queued before updateExcludedPaths() includes it',
    async function() {
      await fse.ensureDir(path.join(watchedPath, 'x'));
      await fse.ensureDir(path.join(watchedPath, 'q'));
      touch(path.join('x', 'gone.file'));
      const excludedPath = path.join(watchedPath, 'q');

      await watchWhile(async (events, watch) => {
        await untilLoopWaits('nsfw-test-pause-before-lock', 'nsfw-test-resume-event-loop');
        // x still has its node, and its watch, when q's scan finds it.
        fs.renameSync(path.join(watchedPath, 'x'), path.join(excludedPath, 'x'));
        fs.writeFileSync(path.join(excludedPath, 'x', 'secret.file'), 'Secret');
        fs.unlinkSync(path.join(excludedPath, 'x', 'gone.file'));
        await watch.updateExcludedPaths([]);
        touch('nsfw-test-resume-event-loop');

        await barrier(events);
        await fse.writeFile(path.join(excludedPath, 'x', 'top.file'), 'Top');
        const created = event => event.action === nsfw.actions.CREATED &&
          event.directory === path.join(excludedPath, 'x') && event.file === 'top.file';
        await waitFor(events, created, 'creation in x');
        // What x holds once it's watched again is reported as created, but not what happened in it while excluded.
        assert.deepStrictEqual(
          events.filter(event => event.file === 'secret.file' && event.action !== nsfw.actions.CREATED), []);
        assert.deepStrictEqual(events.filter(event => event.file === 'gone.file'), []);
      }, [path.join(watchedPath, 'q')]);
    });

  it('does not report what a directory moved into an excluded one queued, once its old parent is seen elsewhere',
    async function() {
      await fse.ensureDir(path.join(watchedPath, 'a', 'b'));
      await fse.ensureDir(path.join(watchedPath, 'q'));
      touch(path.join('a', 'b', 'gone.file'));
      const excludedPath = path.join(watchedPath, 'q');

      await watchWhile(async (events, watch) => {
        await untilLoopWaits('nsfw-test-pause-before-lock', 'nsfw-test-resume-event-loop');
        // z's scan sees a there after q's scan saw b: b, which a takes along, is seen again by z's rescan of it.
        fs.mkdirSync(path.join(watchedPath, 'z'));
        fs.renameSync(path.join(watchedPath, 'a'), path.join(watchedPath, 'z', 'a'));
        fs.renameSync(path.join(watchedPath, 'z', 'a', 'b'), path.join(excludedPath, 'b'));
        fs.unlinkSync(path.join(excludedPath, 'b', 'gone.file'));
        await watch.updateExcludedPaths([]);
        touch('nsfw-test-resume-event-loop');

        await barrier(events);
        await fse.writeFile(path.join(excludedPath, 'b', 'top.file'), 'Top');
        const created = event => event.action === nsfw.actions.CREATED &&
          event.directory === path.join(excludedPath, 'b') && event.file === 'top.file';
        await waitFor(events, created, 'creation in b');
        assert.deepStrictEqual(events.filter(event => event.file === 'gone.file'), []);
      }, [path.join(watchedPath, 'q')]);
    });

  // updateExcludedPaths() holds the lock that keeps the event loop from reading. The directories it finds again with
  // their old watches share one marker, queued once all of them were found: a marker per path removes another dropped
  // watch each time, so thousands of paths queue thousands of IN_IGNORED events, past the queue's limit.
  it('queues one marker for every directory updateExcludedPaths() includes again', async function() {
    const names = ['a1', 'a2', 'a3', 'd1', 'd2', 'd3'];
    for (const name of names) {
      await fse.ensureDir(path.join(watchedPath, name));
    }

    await watchWhile(async (events, watch) => {
      await untilLoopWaits('nsfw-test-pause-before-lock', 'nsfw-test-resume-event-loop');
      // Still queued, so neither call removes dropped watches on its own: only markers do.
      touch('before.file');
      await watch.updateExcludedPaths(names.map(name => path.join(watchedPath, name)));
      const watches = countInotifyWatches();
      await watch.updateExcludedPaths(['a1', 'a2', 'a3'].map(name => path.join(watchedPath, name)));
      // d1, d2 and d3 take their watches back, and the marker is the removal of one of a1's, a2's and a3's.
      assert.strictEqual(countInotifyWatches(), watches - 1);
      touch('nsfw-test-resume-event-loop');

      await barrier(events);
      await expectWatched(events, ['d1'], ['d2'], ['d3']);
    });
  });

  it('finishes deferred descendant scans before a re-inclusion resolves', async function() {
    fs.writeFileSync(markerSignal, '');
    const parent = path.join(watchedPath, 'parent');
    const children = ['first', 'second'].map(name => path.join(parent, name));
    for (const child of children) {
      await fse.ensureDir(child);
      await fse.writeFile(path.join(child, 'existing.file'), '');
    }

    await watchWhile(async (events, watch) => {
      await untilLoopWaits('nsfw-test-pause-before-lock', 'nsfw-test-resume-event-loop');
      touch('before.file');
      let update;
      try {
        await watch.updateExcludedPaths([parent]);
        let resolved = false;
        update = watch.updateExcludedPaths([]).then(paths => {
          resolved = true;
          return paths;
        });
        await untilOneMarker();
        assert.strictEqual(resolved, false);
      } finally {
        touch('nsfw-test-resume-event-loop');
      }
      assert.deepStrictEqual(await update, []);
      for (const child of children) fs.writeFileSync(path.join(child, 'after.file'), '');
      for (const child of children) {
        await waitFor(events, event => event.action === nsfw.actions.CREATED &&
          event.directory === child && event.file === 'after.file', 'creation after re-inclusion');
      }
      assert.deepStrictEqual(events.filter(event => event.file === 'existing.file'), []);
    });
  });

  it('finishes restoring a link with no watched real entry before a re-inclusion resolves', async function() {
    fs.writeFileSync(markerSignal, '');
    const portal = path.join(watchedPath, 'portal');
    const blocked = path.join(portal, 'blocked');
    const alias = path.join(watchedPath, 'alias');
    const target = path.join(outsidePath, 'blocked');
    const parent = path.join(watchedPath, 'P');
    await fse.ensureDir(target);
    await fse.writeFile(path.join(target, 'existing.file'), '');
    await fse.symlink(outsidePath, portal);
    await fse.symlink(target, alias);
    await fse.ensureDir(path.join(parent, 'first'));
    await fse.ensureDir(path.join(parent, 'second'));

    await watchWhile(async (events, watch) => {
      await untilLoopWaits('nsfw-test-pause-before-lock', 'nsfw-test-resume-event-loop');
      touch('before.file');
      let update;
      try {
        await watch.updateExcludedPaths([portal, blocked, parent]);
        update = watch.updateExcludedPaths([portal]);
        await untilOneMarker();
      } finally {
        touch('nsfw-test-resume-event-loop');
      }
      await update;
      fs.writeFileSync(path.join(target, 'after.file'), '');
      await waitFor(events, event => event.action === nsfw.actions.CREATED && event.directory === alias &&
        event.file === 'after.file', 'creation immediately after re-inclusion');
      assert.deepStrictEqual(events.filter(event => event.file === 'existing.file'), []);
    }, [portal, blocked]);
  });

  // Each read gives one pass its marker, so the directories found again below parent take a read each: the one that
  // removes the directory of the only skipped link comes while they are still pending, and leaves no link to restore.
  it('resolves a re-inclusion whose skipped links went while their restoration waited', async function() {
    fs.writeFileSync(markerSignal, '');
    const portal = path.join(watchedPath, 'portal');
    const blocked = path.join(portal, 'blocked');
    const holder = path.join(watchedPath, 'holder');
    const target = path.join(outsidePath, 'blocked');
    const parent = path.join(watchedPath, 'P');
    await fse.ensureDir(target);
    await fse.ensureDir(holder);
    await fse.symlink(outsidePath, portal);
    await fse.symlink(target, path.join(holder, 'alias'));
    await fse.ensureDir(path.join(parent, 'a', 'b', 'c', 'd'));

    await watchWhile(async (events, watch) => {
      await untilLoopWaits('nsfw-test-pause-before-lock', 'nsfw-test-resume-event-loop');
      touch('before.file');
      let update;
      try {
        await watch.updateExcludedPaths([portal, blocked, parent]);
        update = watch.updateExcludedPaths([portal]);
        await untilOneMarker();
        fs.rmSync(holder, { recursive: true });
      } finally {
        touch('nsfw-test-resume-event-loop');
      }
      const resolved = await Promise.race([update.then(() => true), sleep(20000).then(() => false)]);
      assert.ok(resolved, 'The update never resolved');
      await expectWatched(events, ['P', 'a', 'b', 'c', 'd']);
    }, [portal, blocked]);
  });

  // parent is found again with its dropped watch, and the one marker of the pass goes to first: second, found again
  // by parent's scan, waits for the next read with its events dropped. Resolves once it does, with the update still
  // pending in a list: awaiting it here would wait for the event loop that the caller has yet to resume.
  async function untilSecondAwaitsMarker(watch, parent, blocked) {
    await watch.updateExcludedPaths([blocked, parent]);
    const update = watch.updateExcludedPaths([blocked]);
    await untilOneMarker();
    return [update];
  }

  for (const dropped of [true, false]) {
    const arrival = dropped ? 'is dropped with its quarantined watch' : 'is left to the scan after its marker';
    it(`drops a link into an excluded directory whose arrival ${arrival}`, async function() {
      fs.writeFileSync(markerSignal, '');
      const parent = path.join(watchedPath, 'parent');
      const blocked = path.join(parent, 'second', 'blocked');
      const original = path.join(outsidePath, 'original');
      const moving = path.join(watchedPath, 'moving.file');
      await fse.ensureDir(path.join(parent, 'first'));
      await fse.ensureDir(path.join(parent, 'second'));
      await fse.ensureDir(path.join(original, 'deep'));
      await fse.symlink(path.join(original, 'deep'), path.join(watchedPath, 'a-link'));
      await fse.writeFile(moving, '');

      await watchWhile(async (events, watch) => {
        assert.deepStrictEqual(await reportedIn(events, original, ['deep/before.file']), ['a-link/before.file']);
        if (dropped) {
          // The read that pauses ends with half of a move, so second isn't given its marker after it: the arrival
          // and the write at the link are read while its events are still dropped.
          await untilLoopWaits('nsfw-test-hold-event-loop', 'nsfw-test-release-event-loop');
          await fse.remove(waitingPath);
          touch('nsfw-test-pause-before-lock');
          touch('nsfw-test-release-event-loop');
          fs.renameSync(moving, path.join(outsidePath, 'moved.file'));
          for (let i = 0; i < 200 && !fs.existsSync(waitingPath); ++i) await sleep(50);
          assert.ok(fs.existsSync(waitingPath), 'The event loop never paused');
        } else {
          await untilLoopWaits('nsfw-test-pause-before-lock', 'nsfw-test-resume-event-loop');
        }
        touch('before.file');
        let update;
        try {
          [update] = await untilSecondAwaitsMarker(watch, parent, blocked);
          fs.renameSync(original, blocked);
          fs.writeFileSync(path.join(blocked, 'deep', 'queued.file'), '');
        } finally {
          touch('nsfw-test-resume-event-loop');
        }
        await update;
        await barrier(events);
        assert.deepStrictEqual(events.filter(event => event.file === 'queued.file'), []);
        assert.deepStrictEqual(await reportedIn(events, blocked, ['deep/later.file']), []);
        await expectWatched(events, ['parent', 'second']);
      }, [blocked]);
    });
  }

  it('restores a link once its pending parent rename is handled', async function() {
    const portal = path.join(watchedPath, 'portal');
    const blocked = path.join(portal, 'blocked');
    const target = path.join(outsidePath, 'blocked');
    const parent = path.join(watchedPath, 'container');
    const moved = path.join(watchedPath, 'moved');
    await fse.ensureDir(parent);
    await fse.ensureDir(target);
    await fse.symlink(outsidePath, portal);
    await fse.symlink(target, path.join(parent, 'link'));

    await watchWhile(async (events, watch) => {
      await untilLoopWaits('nsfw-test-pause-before-lock', 'nsfw-test-resume-event-loop');
      try {
        fs.renameSync(parent, moved);
        await watch.updateExcludedPaths([portal]);
      } finally {
        touch('nsfw-test-resume-event-loop');
      }
      await barrier(events);
      fs.writeFileSync(path.join(target, 'after.file'), '');
      await waitFor(events, event => event.action === nsfw.actions.CREATED &&
        event.directory === path.join(moved, 'link') && event.file === 'after.file', 'creation after parent rename');
    }, [portal, blocked]);
  });

  it('stops a watcher with an unfinished re-inclusion and can restart with a later update', async function() {
    fs.writeFileSync(markerSignal, '');
    const parent = path.join(watchedPath, 'parent');
    const child = path.join(parent, 'child');
    await fse.ensureDir(child);
    await fse.ensureDir(path.join(parent, 'sibling'));
    await watchWhile(async (events, watch) => {
      await untilLoopWaits('nsfw-test-pause-before-lock', 'nsfw-test-resume-event-loop');
      touch('before.file');
      await watch.updateExcludedPaths([parent]);
      let resolved = false;
      const update = watch.updateExcludedPaths([]).then(paths => {
        resolved = true;
        return paths;
      });
      try {
        await untilOneMarker();
        assert.strictEqual(resolved, false);
        const stop = watch.stop();
        const start = watch.start();
        const laterUpdate = watch.updateExcludedPaths([child]);
        assert.strictEqual(await Promise.race([stop.then(() => true), sleep(5000).then(() => false)]), true);
        assert.deepStrictEqual(await update, []);
        await start;
        assert.deepStrictEqual(await laterUpdate, [child]);
        assert.deepStrictEqual(await watch.getExcludedPaths(), [child]);
      } finally {
        touch('nsfw-test-resume-event-loop');
      }
      await watch.updateExcludedPaths([]);
      await expectWatched(events, ['parent', 'child']);
    });
  });

  it('waits for an earlier deferred move scan to install a re-included descendant', async function() {
    fs.writeFileSync(markerSignal, '');
    const top = path.join(watchedPath, 'x');
    const moved = path.join(watchedPath, 'y');
    const branch = path.join('a', 'b', 'c', 'd', 'e', 'f');
    const child = path.join(moved, branch, 'child');
    await fse.ensureDir(path.join(top, branch, 'child'));
    await fse.writeFile(path.join(top, branch, 'visible.file'), '');
    await fse.writeFile(path.join(top, branch, 'child', 'old.file'), '');
    await watchWhile(async (events, watch) => {
      await untilLoopWaits('nsfw-test-pause-before-lock', 'nsfw-test-resume-event-loop');
      const outside = path.join(outsidePath, 'away');
      fs.renameSync(top, outside);
      fs.renameSync(outside, moved);
      await fse.remove(waitingPath);
      touch('nsfw-test-hold-event-loop');
      touch('nsfw-test-resume-event-loop');
      for (let i = 0; i < 200 && !fs.existsSync(waitingPath); ++i) await sleep(50);
      assert.ok(fs.existsSync(waitingPath), 'The move was never handled');

      // Pause the following read outside the event mutex so the update can see the unfinished scan.
      for (const name of ['nsfw-test-pause-before-lock', 'nsfw-test-resume-event-loop']) {
        await fse.remove(path.join(watchedPath, name));
      }
      await fse.remove(waitingPath);
      touch('nsfw-test-pause-before-lock');
      touch('nsfw-test-release-event-loop');
      try {
        for (let i = 0; i < 200 && !fs.existsSync(waitingPath); ++i) await sleep(50);
        assert.ok(fs.existsSync(waitingPath), 'The next read never paused');
        const update = watch.updateExcludedPaths([]).then(() => {
          fs.writeFileSync(path.join(child, 'after.file'), '');
          return true;
        });
        assert.strictEqual(await Promise.race([update, sleep(500).then(() => false)]), false);
        touch('nsfw-test-resume-event-loop');
        await update;
        await waitFor(events, event => event.action === nsfw.actions.CREATED &&
          event.directory === child && event.file === 'after.file', 'creation immediately after the update');
        await waitFor(events, event => event.file === 'visible.file', 'visible contents of the moved tree');
        assert.deepStrictEqual(events.filter(event => event.file === 'old.file'), []);
      } finally {
        touch('nsfw-test-resume-event-loop');
      }
    }, [child]);
  });

  // A directory found again isn't read until the marker of its pass, and what its old watch queues until then is
  // dropped: a child of it found in the same pass may leave without its parent's events telling.
  it('does not report under its old path what a directory included again queued once it moved, with its parent ' +
    'included again in the same call', async function() {
    await fse.ensureDir(path.join(watchedPath, 'a', 'b'));
    await fse.ensureDir(path.join(watchedPath, 'c'));
    await fse.ensureDir(path.join(watchedPath, 'x'));
    const at = relative => path.join(watchedPath, ...relative.split('/'));

    await watchWhile(async (events, watch) => {
      await untilLoopWaits('nsfw-test-pause-before-lock', 'nsfw-test-resume-event-loop');
      touch('before.file');
      await watch.updateExcludedPaths(['x', 'a', 'a/b', 'c'].map(at));
      // The slow build takes 500 ms per node. a is found again at about 500 ms and c at 1000, when their marker is
      // queued, and only a's scan, after it, finds b: b's node isn't watching by 1250 ms. One that built b below a
      // right away, before c, would have its watch by then, behind the same marker as a.
      const update = watch.updateExcludedPaths([at('x')]);
      await sleep(1250);
      fs.renameSync(at('a/b'), at('x/b'));
      await update;
      touch('nsfw-test-resume-event-loop');

      await barrier(events);
      await fse.writeFile(at('x/b/inside.file'), 'Inside');
      await sleep(1000);
      await barrier(events);
      assert.deepStrictEqual(events.filter(event => event.directory.startsWith(at('a/b'))), []);
    }, [path.join(watchedPath, 'x')]);
  });

  // A move that takes directories out of the paths excluded below it includes them again while its one event is
  // handled: like updateExcludedPaths(), the ones found again with their old watches share one marker.
  it('queues one marker for every directory a move includes again', async function() {
    for (const name of ['a1', 'a2', 'a3', 'x/k1/nm', 'x/k2/nm', 'x/k3/nm']) {
      await fse.ensureDir(path.join(watchedPath, ...name.split('/')));
    }
    const at = relative => path.join(watchedPath, ...relative.split('/'));

    await watchWhile(async (events, watch) => {
      await untilLoopWaits('nsfw-test-pause-before-lock', 'nsfw-test-resume-event-loop');
      // Still queued, so the read the loop holds removes no dropped watch once handled: only markers do.
      touch('before.file');
      await watch.updateExcludedPaths(['a1', 'a2', 'a3', 'x/k1/nm', 'x/k2/nm', 'x/k3/nm'].map(at));
      const watches = countInotifyWatches();
      // y/k1/nm, y/k2/nm and y/k3/nm aren't excluded: the move includes them again, with their old watches. The
      // hold file, after the move in the same read, keeps the loop there once the move is handled.
      fs.renameSync(at('x'), at('y'));
      await fse.remove(waitingPath);
      touch('nsfw-test-hold-event-loop');
      touch('nsfw-test-resume-event-loop');
      for (let i = 0; i < 200 && !fs.existsSync(waitingPath); ++i) {
        await sleep(50);
      }
      assert.ok(fs.existsSync(waitingPath), 'The event loop never waited at the hold file');
      // The marker is the removal of one of a1's, a2's and a3's dropped watches.
      assert.strictEqual(countInotifyWatches(), watches - 1);
      touch('nsfw-test-release-event-loop');

      await barrier(events);
      await expectWatched(events, ['y', 'k1', 'nm'], ['y', 'k2', 'nm'], ['y', 'k3', 'nm']);
    });
  });

  // A directory found again is scanned after its marker: one below it that a move includes again in the same
  // handling is found by that scan, not before, see createChild().
  it('watches nested directories a move includes again', async function() {
    await fse.ensureDir(path.join(watchedPath, 'x', 'a', 'b'));
    const at = relative => path.join(watchedPath, ...relative.split('/'));

    await watchWhile(async (events, watch) => {
      await untilLoopWaits('nsfw-test-pause-before-lock', 'nsfw-test-resume-event-loop');
      touch('before.file');
      await watch.updateExcludedPaths(['x/a', 'x/a/b'].map(at));
      fs.renameSync(at('x'), at('y'));
      touch('nsfw-test-resume-event-loop');

      await barrier(events);
      await expectWatched(events, ['y', 'a'], ['y', 'a', 'b']);
    });
  });

  // A directory updateExcludedPaths() finds again is scanned after the marker its pass queues
  // once the whole call is done. Meanwhile it may move on, and another directory take its path: that one's contents
  // must not hang below the node, which the queued rename then moves with them.
  it('does not watch what a directory that took the path of one included again holds, below that one',
    async function() {
      await fse.ensureDir(path.join(watchedPath, 'a'));
      await fse.ensureDir(path.join(watchedPath, 'c'));
      await fse.ensureDir(path.join(watchedPath, 'e'));
      await fse.ensureDir(path.join(outsidePath, 'X', 'k'));
      const at = relative => path.join(watchedPath, ...relative.split('/'));

      await watchWhile(async (events, watch) => {
        await untilLoopWaits('nsfw-test-pause-before-lock', 'nsfw-test-resume-event-loop');
        touch('before.file');
        await watch.updateExcludedPaths(['a', 'c', 'e'].map(at));
        // The slow build takes 500 ms per node: a is found again at about 500 ms, c at 1000 and e at 1500.
        const update = watch.updateExcludedPaths([]);
        await sleep(750);
        fs.renameSync(at('a'), at('other'));
        fs.renameSync(path.join(outsidePath, 'X'), at('a'));
        touch('nsfw-test-resume-event-loop');
        await update;

        await barrier(events);
        await expectWatched(events, ['a', 'k']);
        await barrier(events);
        assert.deepStrictEqual(events.filter(event => event.directory.startsWith(at('other'))), []);
      });
    });

  // L1, with the marker a dropped watch: z stays excluded, so its watch, still waiting to be removed, is the marker,
  // and a is scanned by its path with no check of what's there now (rewatch() has one, see the spec above).
  it('does not watch what a directory that took the path of one included again holds, with another marker',
    async function() {
      await fse.ensureDir(path.join(watchedPath, 'a'));
      await fse.ensureDir(path.join(watchedPath, 'c'));
      await fse.ensureDir(path.join(watchedPath, 'e'));
      await fse.ensureDir(path.join(watchedPath, 'z'));
      await fse.ensureDir(path.join(outsidePath, 'X', 'k'));
      const at = relative => path.join(watchedPath, ...relative.split('/'));

      await watchWhile(async (events, watch) => {
        await untilLoopWaits('nsfw-test-pause-before-lock', 'nsfw-test-resume-event-loop');
        touch('before.file');
        await watch.updateExcludedPaths(['a', 'c', 'e', 'z'].map(at));
        // The slow build takes 500 ms per node: a is found again at about 500 ms, c at 1000 and e at 1500.
        const update = watch.updateExcludedPaths([at('z')]);
        await sleep(750);
        fs.renameSync(at('a'), at('other'));
        fs.renameSync(path.join(outsidePath, 'X'), at('a'));
        await update;
        touch('nsfw-test-resume-event-loop');

        await barrier(events);
        await expectWatched(events, ['a', 'k']);
        await barrier(events);
        assert.deepStrictEqual(events.filter(event => event.directory.startsWith(at('other'))), []);
      });
    });

  it('preserves an existing watch when another directory replaces a child being installed', async function() {
    await fse.ensureDir(path.join(watchedPath, 'P', 'child'));
    await fse.ensureDir(path.join(watchedPath, 'S'));
    const child = path.join(watchedPath, 'Q', 'child');
    await watchWhile(async events => {
      fs.renameSync(path.join(watchedPath, 'P'), path.join(watchedPath, 'Q'));
      await waitFor(events, event => event.action === nsfw.actions.RENAMED &&
        event.oldFile === 'P' && event.newFile === 'Q', 'parent rename before child installation');
      fs.renameSync(child, path.join(outsidePath, 'original'));
      fs.renameSync(path.join(watchedPath, 'S'), child);
      await barrier(events);
      await expectWatched(events, ['Q', 'child']);
    }, [path.join(watchedPath, 'P', 'child')]);
  });

  // c is found again at about 1000 ms and its pass settles at about 2000, once e and g are found
  // too; z stays excluded, so its dropped watch is the marker and c is scanned by its path. At 1500 ms c moves on and
  // another directory takes its path: the scan must not hang that one's k below c's node, which the queued rename
  // takes to other. other/sub then checks that c's node, moved there, still watches what is created in it.
  it('does not scan a directory found again whose path leads to another one by then', async function() {
    for (const name of ['a', 'c', 'e', 'g', 'z']) {
      await fse.ensureDir(path.join(watchedPath, name));
    }
    await fse.writeFile(path.join(watchedPath, 'c', 'old.file'), 'Old');
    await fse.ensureDir(path.join(outsidePath, 'X', 'k'));
    const at = relative => path.join(watchedPath, ...relative.split('/'));

    await watchWhile(async (events, watch) => {
      await untilLoopWaits('nsfw-test-pause-before-lock', 'nsfw-test-resume-event-loop');
      touch('before.file');
      await watch.updateExcludedPaths(['a', 'c', 'e', 'g', 'z'].map(at));
      const update = watch.updateExcludedPaths([at('z')]);
      await sleep(1500);
      fs.renameSync(at('c'), at('other'));
      fs.renameSync(path.join(outsidePath, 'X'), at('c'));
      await update;
      touch('nsfw-test-resume-event-loop');

      await barrier(events);
      await expectWatched(events, ['c', 'k']);
      await barrier(events);
      assert.deepStrictEqual(events.filter(event => event.directory.startsWith(at('other/k'))), []);
      fs.mkdirSync(at('other/sub'));
      await expectWatched(events, ['other', 'sub']);
      // updateExcludedPaths() reports nothing of what a directory it includes again holds.
      assert.deepStrictEqual(events.filter(event => event.action === nsfw.actions.CREATED &&
        event.file === 'old.file'), []);
    });
  });

  // d1 comes back inside Q and is found again by Q's scan; sub's dropped watch is the marker of its pass, so d1's own
  // scan waits until Q's scan is done. Q/m1 is reported as Q's scan reaches it, after d1 and with m1 to m4 still to
  // add (500 ms each in the slow build), so d1 moves on before its scan. Its rescan after that rename reports what it
  // held, once and under its new path; a scan of it before the move would report it under Q/d1.
  it('reports what a directory found again holds when it moves on before its scan', async function() {
    await fse.ensureDir(path.join(watchedPath, 'd1', 'sub'));
    await fse.writeFile(path.join(watchedPath, 'd1', 'old.file'), 'Old');
    await fse.writeFile(path.join(watchedPath, 'd1', 'sub', 'deep.file'), 'Deep');
    for (const name of ['m1', 'm2', 'm3', 'm4']) {
      await fse.ensureDir(path.join(outsidePath, 'Q', name));
    }
    const at = relative => path.join(watchedPath, ...relative.split('/'));

    await watchWhile(async events => {
      await whileHeld(async () => {
        fs.renameSync(at('d1'), path.join(outsidePath, 'Q', 'd1'));
        fs.renameSync(path.join(outsidePath, 'Q'), at('Q'));
      });
      await waitFor(events, event => event.action === nsfw.actions.CREATED && event.directory === at('Q') &&
        event.file === 'm1', 'creation of Q/m1');
      fs.renameSync(at('Q/d1'), at('E'));
      await barrier(events);
      const renamed = events.findIndex(event => event.action === nsfw.actions.RENAMED &&
        event.directory === at('Q') && event.oldFile === 'd1' && event.newDirectory === watchedPath &&
        event.newFile === 'E');
      assert.ok(renamed >= 0, `No rename of Q/d1 in ${JSON.stringify(events)}`);
      for (const [directory, file] of [['E', 'old.file'], ['E', 'sub'], ['E/sub', 'deep.file']]) {
        const creations = events.map((event, index) => ({ event, index })).filter(({ event }) =>
          event.action === nsfw.actions.CREATED && event.file === file);
        assert.deepStrictEqual(creations.map(({ event }) => event.directory), [at(directory)],
          `Creations of ${file} in ${JSON.stringify(events)}`);
        assert.ok(creations[0].index > renamed, `${file} reported before the rename in ${JSON.stringify(events)}`);
      }
      await expectWatched(events, ['E'], ['E', 'sub']);
    });
  });

  // A directory that leaves while a scan saw the directories it held elsewhere: they are watched there, in one pass
  // like any other that finds directories again with their old watches, so one marker for all of them.
  it('queues one marker for the directories a scan saw elsewhere once the directory they left goes',
    async function() {
      for (const name of ['a1', 'a2', 'a3', 'd1', 'd2', 'd3']) {
        await fse.ensureDir(path.join(watchedPath, 'S', name));
      }

      await watchWhile(async events => {
        await untilLoopWaits('nsfw-test-pause-before-lock', 'nsfw-test-resume-event-loop');
        const watches = countInotifyWatches();
        // Q's scan sees d1, d2 and d3 before S's move out is handled: S's discard watches them in Q, with the
        // watches they had. The hold file, after them in the same read, keeps the loop there once they're handled.
        fs.mkdirSync(path.join(watchedPath, 'Q'));
        fs.renameSync(path.join(watchedPath, 'S'), path.join(outsidePath, 'S'));
        for (const name of ['d1', 'd2', 'd3']) {
          fs.renameSync(path.join(outsidePath, 'S', name), path.join(watchedPath, 'Q', name));
        }
        await fse.remove(waitingPath);
        touch('nsfw-test-hold-event-loop');
        touch('nsfw-test-resume-event-loop');
        for (let i = 0; i < 200 && !fs.existsSync(waitingPath); ++i) {
          await sleep(50);
        }
        assert.ok(fs.existsSync(waitingPath), 'The event loop never waited at the hold file');
        // Q's new watch, and S's dropped one removed as the marker.
        assert.strictEqual(countInotifyWatches(), watches);
        touch('nsfw-test-release-event-loop');

        await barrier(events);
        await expectWatched(events, ['Q', 'd1'], ['Q', 'd2'], ['Q', 'd3']);
      });
    });

  it('watches a directory moved back when a namesake is created in an excluded one',
    async function() {
      await fse.ensureDir(path.join(watchedPath, 'src', 'D'));
      await fse.ensureDir(path.join(watchedPath, 'node_modules', 'pkg'));
      touch(path.join('src', 'D', 'existing.file'));

      await watchWhile(async (events, watch) => {
        await untilLoopWaits('nsfw-test-pause-before-lock', 'nsfw-test-resume-event-loop');
        // Still queued when node_modules is excluded, so pkg's watch stays: src's creation comes under it.
        touch('before.file');
        await watch.updateExcludedPaths([path.join(watchedPath, 'node_modules')]);
        fs.renameSync(path.join(watchedPath, 'src', 'D'), path.join(outsidePath, 'D'));
        fs.renameSync(path.join(outsidePath, 'D'), path.join(watchedPath, 'src', 'D'));
        fs.mkdirSync(path.join(watchedPath, 'node_modules', 'pkg', 'src'));
        fs.unlinkSync(path.join(watchedPath, 'src', 'D', 'existing.file'));
        touch('nsfw-test-resume-event-loop');

        await checkReturnState(events, path.join(watchedPath, 'src', 'D'), 'existing.file', false, true);
      });
    });

  it('does not report what an excluded directory queued once it moved into a directory that arrived before it',
    async function() {
      await fse.ensureDir(path.join(watchedPath, 'E', 'D'));
      await fse.ensureDir(path.join(outsidePath, 'X'));

      await watchWhile(async (events, watch) => {
        await untilLoopWaits('nsfw-test-pause-before-lock', 'nsfw-test-resume-event-loop');
        // Still queued when E is excluded, so D's watch stays.
        touch('before.file');
        await watch.updateExcludedPaths([path.join(watchedPath, 'E')]);
        fs.renameSync(path.join(outsidePath, 'X'), path.join(watchedPath, 'X'));
        fs.writeFileSync(path.join(watchedPath, 'E', 'D', 'secret.file'), 'Secret');
        // X isn't watched yet, so nothing tells when D arrived there.
        fs.renameSync(path.join(watchedPath, 'E', 'D'), path.join(watchedPath, 'X', 'D'));
        touch('nsfw-test-resume-event-loop');

        await barrier(events);
        await fse.writeFile(path.join(watchedPath, 'X', 'D', 'top.file'), 'Top');
        const created = event => event.action === nsfw.actions.CREATED &&
          event.directory === path.join(watchedPath, 'X', 'D') && event.file === 'top.file';
        await waitFor(events, created, 'creation in D');
        assert.deepStrictEqual(
          events.filter(event => event.file === 'secret.file' && event.action !== nsfw.actions.CREATED), []);
      });
    });

  // Nothing orders a directory's arrival below one that wasn't watched yet against the events it queued before, so
  // it's watched like one that moves in for the first time.
  for (const [description, away, wrapperFromOutside] of [
    ['into an excluded directory', ['E', 'D'], true],
    ['out', null, true],
    ['out, below a directory created and renamed in the watched path', null, false]
  ]) {
    it(`does not report what a directory moved ${description} queued while it couldn't be watched once it moved ` +
      'into a directory that arrived before it', async function() {
      await fse.ensureDir(path.join(watchedPath, 'D'));
      touch(path.join('D', 'secret.file'));
      await fse.ensureDir(path.join(watchedPath, 'E'));
      await fse.ensureDir(path.join(outsidePath, 'X'));
      const awayPath = away ? path.join(watchedPath, ...away) : path.join(outsidePath, 'D');
      const wrapperPath = path.join(watchedPath, 'X');

      await watchWhile(async events => {
        await whileHeld(async () => {
          if (!wrapperFromOutside) {
            fs.mkdirSync(path.join(watchedPath, 'W'));
          }
          // Left from where it was watched, so its watch stays a while, as one that may come back.
          fs.renameSync(path.join(watchedPath, 'D'), awayPath);
          fs.renameSync(wrapperFromOutside ? path.join(outsidePath, 'X') : path.join(watchedPath, 'W'), wrapperPath);
          fs.unlinkSync(path.join(awayPath, 'secret.file'));
          // X isn't watched yet, so nothing tells when D arrived there.
          fs.renameSync(awayPath, path.join(wrapperPath, 'D'));
        });

        await barrier(events);
        await fse.writeFile(path.join(wrapperPath, 'D', 'top.file'), 'Top');
        await waitFor(events, event => event.action === nsfw.actions.CREATED &&
          event.directory === path.join(wrapperPath, 'D') && event.file === 'top.file', 'creation in D');
        assert.deepStrictEqual(events.filter(event => event.file === 'secret.file'), []);
      }, [path.join(watchedPath, 'E')]);
    });
  }

  // What a late return loses, against what one that came back before its watches were removed keeps (see
  // 'reports a deletion in a directory moved out and back in within one read, right after'): the same as a directory
  // that moves in for the first time.
  for (const left of [true, false]) {
    it('reports what a directory holds when it comes back after its watches were removed, like one that ' +
      `${left ? 'left' : 'was never watched'}`, async function() {
      const existing = left ? path.join(watchedPath, 'd') : path.join(outsidePath, 'd');
      await fse.ensureDir(path.join(existing, 'trip'));
      for (const file of ['existing.file', 'other.file']) {
        fs.writeFileSync(path.join(existing, 'trip', file), 'Existing');
      }
      const otherWatches = countInotifyWatches();

      await watchWhile(async events => {
        if (left) {
          fs.renameSync(path.join(watchedPath, 'd'), path.join(outsidePath, 'd'));
          await waitFor(events, event => event.action === nsfw.actions.DELETED && event.file === 'd',
            'departure');
          // Only the watched path's is left.
          await watchCountReaches(otherWatches + 1, 200);
        }
        await whileHeld(async () => {
          fs.renameSync(path.join(outsidePath, 'd'), path.join(watchedPath, 'e'));
          fs.unlinkSync(path.join(watchedPath, 'e', 'trip', 'existing.file'));
        });

        const tripPath = path.join(watchedPath, 'e', 'trip');
        const created = (directory, file) => event => event.action === nsfw.actions.CREATED &&
          event.directory === directory && event.file === file;
        await waitFor(events, created(tripPath, 'other.file'), 'creation of what it holds');
        await fse.remove(path.join(tripPath, 'other.file'));
        await waitFor(events, event => event.action === nsfw.actions.DELETED && event.directory === tripPath &&
          event.file === 'other.file', 'deletion inside');
        await barrier(events);

        assert.ok(events.some(created(watchedPath, 'e')));
        assert.ok(events.some(created(path.join(watchedPath, 'e'), 'trip')));
        assert.deepStrictEqual({
          existing: events.filter(event => event.file === 'existing.file'),
          oldPath: events.filter(event => event.directory.startsWith(path.join(watchedPath, 'd')))
        }, { existing: [], oldPath: [] });
        // The watched path, e and trip.
        assert.strictEqual(countInotifyWatches(), otherWatches + 3);
      });
    });
  }

  it('watches an excluded directory moved to a visible path after a deletion', async function() {
    await fse.ensureDir(path.join(watchedPath, 'x', 'd'));
    touch(path.join('x', 'd', 'existing.file'));

    await watchWhile(async (events, watch) => {
      await untilLoopWaits('nsfw-test-pause-before-lock', 'nsfw-test-resume-event-loop');
      touch('before.file');
      await watch.updateExcludedPaths([path.join(watchedPath, 'x')]);
      fs.renameSync(path.join(watchedPath, 'x', 'd'), path.join(watchedPath, 'e'));
      fs.unlinkSync(path.join(watchedPath, 'e', 'existing.file'));
      touch('nsfw-test-resume-event-loop');

      await checkReturnState(events, path.join(watchedPath, 'e'), 'existing.file', false);
    });
  });

  it('does not report what a directory queued outside once it moved into one found behind a marker',
    async function() {
      await fse.ensureDir(path.join(watchedPath, 'a', 'D'));
      await fse.ensureDir(path.join(watchedPath, 'E'));
      await fse.ensureDir(path.join(outsidePath, 'x'));
      fs.writeFileSync(path.join(watchedPath, 'E', 'secret.file'), 'Secret');

      await watchWhile(async (events, watch) => {
        await untilLoopWaits('nsfw-test-pause-before-lock', 'nsfw-test-resume-event-loop');
        touch('before.file');
        await watch.updateExcludedPaths([path.join(watchedPath, 'a', 'D')]);
        fs.renameSync(path.join(watchedPath, 'E'), path.join(outsidePath, 'E'));
        fs.renameSync(path.join(watchedPath, 'a', 'D'), path.join(outsidePath, 'x', 'D'));
        // x's scan finds D, which gets a new watch behind a marker, and E below it before E's move in is read.
        fs.renameSync(path.join(outsidePath, 'x'), path.join(watchedPath, 'x'));
        fs.unlinkSync(path.join(outsidePath, 'E', 'secret.file'));
        fs.renameSync(path.join(outsidePath, 'E'), path.join(watchedPath, 'x', 'D', 'E'));
        touch('nsfw-test-resume-event-loop');

        await barrier(events);
        await fse.writeFile(path.join(watchedPath, 'x', 'D', 'E', 'top.file'), 'Top');
        const created = event => event.action === nsfw.actions.CREATED &&
          event.directory === path.join(watchedPath, 'x', 'D', 'E') && event.file === 'top.file';
        await waitFor(events, created, 'creation in E');
        assert.deepStrictEqual(events.filter(event => event.file === 'secret.file'), []);
      });
    });

  it('does not report what a directory queued in an excluded one once the move out of its old place is handled',
    async function() {
      await fse.ensureDir(path.join(watchedPath, 'old', 'H', 'sub'));
      await fse.ensureDir(path.join(watchedPath, 'q'));
      const excludedPath = path.join(watchedPath, 'q');

      await watchWhile(async (events, watch) => {
        await untilLoopWaits('nsfw-test-pause-before-lock', 'nsfw-test-resume-event-loop');
        fs.renameSync(path.join(watchedPath, 'old', 'H'), path.join(excludedPath, 'H'));
        // Its events end the move out of old, whose handling finds H again, below q, behind a marker.
        touch(path.join('q', 'H', 'sub', 'phantom.file'));
        fs.unlinkSync(path.join(excludedPath, 'H', 'sub', 'phantom.file'));
        await watch.updateExcludedPaths([]);
        touch('nsfw-test-resume-event-loop');

        await barrier(events);
        await fse.writeFile(path.join(excludedPath, 'H', 'sub', 'top.file'), 'Top');
        const created = event => event.action === nsfw.actions.CREATED &&
          event.directory === path.join(excludedPath, 'H', 'sub') && event.file === 'top.file';
        await waitFor(events, created, 'creation in sub');
        assert.deepStrictEqual(events.filter(event => event.file === 'phantom.file'), []);
      }, [excludedPath]);
    });

  it('does not report what a directory queued outside once it moved into a removed watch read reads later',
    async function() {
      await fse.ensureDir(path.join(watchedPath, 'R', 'P'));
      await fse.ensureDir(path.join(watchedPath, 'C'));

      await watchWhile(async (events, watch) => {
        await untilLoopWaits('nsfw-test-pause-before-lock', 'nsfw-test-resume-event-loop');
        touch('before.file');
        await watch.updateExcludedPaths([path.join(watchedPath, 'R', 'P')]);
        fs.renameSync(path.join(watchedPath, 'C'), path.join(outsidePath, 'C'));
        // P's watch goes at this event, several reads before the moves below are read.
        touch(path.join('R', 'P', 'poke.file'));
        for (let i = 0; i < 1000; ++i) {
          touch(`filler${i}.file`);
        }
        fs.renameSync(path.join(watchedPath, 'R', 'P'), path.join(watchedPath, 'R', 'Q'));
        fs.writeFileSync(path.join(outsidePath, 'C', 'outside.file'), 'Outside');
        fs.renameSync(path.join(outsidePath, 'C'), path.join(watchedPath, 'R', 'Q', 'C'));
        // More than a read (8 KB) still queued after each read until the moves are read, so C's old watch isn't
        // removed then: each IN_CREATE of a trailer takes 32 bytes, 16 of them the name, so these take 19 KB.
        for (let i = 0; i < 600; ++i) {
          touch(`trailer${i}.file`);
        }
        touch('nsfw-test-resume-event-loop');

        await barrier(events);
        await fse.writeFile(path.join(watchedPath, 'R', 'Q', 'C', 'top.file'), 'Top');
        const created = event => event.action === nsfw.actions.CREATED &&
          event.directory === path.join(watchedPath, 'R', 'Q', 'C') && event.file === 'top.file';
        await waitFor(events, created, 'creation in C');
        assert.deepStrictEqual(
          events.filter(event => event.file === 'outside.file' && event.action !== nsfw.actions.CREATED), []);
      });
    });

  it('watches a directory moved into one that is renamed while it is scanned', async function() {
    await fse.ensureDir(path.join(watchedPath, 'moving', 'inner'));

    await watchWhile(async events => {
      await whileHeld(async () => {
        await fse.ensureDir(path.join(watchedPath, 'archive'));
        await fse.rename(path.join(watchedPath, 'moving'), path.join(watchedPath, 'archive', 'moving'));
      });
      // archive's scan reports moving just before it builds moving's node, which the slow build takes 500 ms to do
      // before adding its watch.
      await waitFor(events, event => event.action === nsfw.actions.CREATED &&
        event.directory === path.join(watchedPath, 'archive') && event.file === 'moving', 'scan of archive');
      await fse.rename(path.join(watchedPath, 'archive'), path.join(watchedPath, 'archived'));
      // Until the rescan after the rename watches it.
      const insideDirectory = path.join(watchedPath, 'archived', 'moving', 'inner');
      const reported = () => events.some(event => event.action === nsfw.actions.CREATED &&
        event.directory === insideDirectory && event.file.startsWith('inside'));
      for (let i = 0; i < 20 && !reported(); ++i) {
        await fse.writeFile(path.join(insideDirectory, `inside-${i}.file`), 'Inside');
        await sleep(500);
      }
      assert.ok(reported(), `No creation inside in ${JSON.stringify(events)}`);
      // Reported under the path the rename gave it, after the rename.
      const insideCreated = events.findIndex(event => event.action === nsfw.actions.CREATED &&
        event.directory === insideDirectory && event.file.startsWith('inside'));
      const renamed = events.findIndex(event => event.action === nsfw.actions.RENAMED &&
        event.newFile === 'archived');
      assert.ok(renamed !== -1 && insideCreated > renamed,
        `No creation inside after the rename in ${JSON.stringify(events)}`);
    });
  });
});
