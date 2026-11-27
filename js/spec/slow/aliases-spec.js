const { beforeEach, describe } = require('node:test');
const assert = require('assert');
const fs = require('fs');
const fsp = require('fs/promises');
const path = require('path');
const nsfw = require('../../dist');
const { WORKDIR: workDir, countInotifyWatches, it, mkdirp, outputFile, remove, sleep } = require('../common');
const fixture = require('./fixture');

describe('inotify aliases (slow)', { skip: process.platform !== 'linux' }, function() {
  const {
    watchedPath, outsidePath, touch, watchWhile, waitFor,
    checkReturnState, waitingPath, untilWaiting, untilLoopWaits, whileHeld, barrier, reportedIn,
    whileScanning, expectWatched
  } = fixture();

  describe('with a real directory excluded', function() {
    it('skips links before or after the excluded entry and links to its descendants', async function() {
      const early = path.join(watchedPath, 'a-blocked');
      const late = path.join(watchedPath, 'z-blocked');
      const sibling = late + '-tools';
      const links = path.join(watchedPath, 'links');
      await Promise.all([early, path.join(late, 'deep'), sibling, links].map(directory => mkdirp(directory)));
      await fsp.symlink(late, path.join(watchedPath, 'b-whole'));
      await fsp.symlink(path.join(late, 'deep'), path.join(watchedPath, 'c-deep'));
      await fsp.symlink(sibling, path.join(watchedPath, 'd-sibling'));
      await fsp.symlink(early, path.join(links, 'late-link'));

      await watchWhile(async events => {
        assert.deepStrictEqual(await reportedIn(events, early, ['early.file']), []);
        assert.deepStrictEqual(await reportedIn(events, late, ['late.file', 'deep/descendant.file']), []);
        assert.deepStrictEqual(await reportedIn(events, sibling, ['sibling.file']), ['d-sibling/sibling.file']);
      }, [early, late]);
    });

    it('keeps the target watched when the excluded entry is itself a symlink', async function() {
      const real = path.join(watchedPath, 'z-real');
      const link = path.join(watchedPath, 'a-link');
      await mkdirp(real);
      await fsp.symlink(real, link);

      await watchWhile(async events => {
        assert.deepStrictEqual(await reportedIn(events, real, ['visible.file']), ['z-real/visible.file']);
      }, [link]);
    });

    it('skips a link to a newly created excluded directory when scanning an arriving subtree', async function() {
      const blocked = path.join(watchedPath, 'blocked');
      const incoming = path.join(outsidePath, 'incoming');
      await mkdirp(incoming);
      await fsp.symlink(path.join(blocked, 'deep'), path.join(incoming, 'link'));

      await watchWhile(async events => {
        await whileHeld(() => {
          fs.mkdirSync(path.join(blocked, 'deep'), { recursive: true });
          fs.renameSync(incoming, path.join(watchedPath, 'incoming'));
        });
        await expectWatched(events, ['incoming']);
        assert.deepStrictEqual(await reportedIn(events, blocked, ['deep/hidden.file']), []);
      }, [blocked]);
    });

    it('drops a link-held destination when a rename gives it the excluded name', async function() {
      const original = path.join(watchedPath, 'z-original');
      const blocked = path.join(watchedPath, 'blocked');
      const hop = path.join(outsidePath, 'hop');
      await mkdirp(path.join(original, 'deep'));
      await fsp.symlink(original, hop);
      await fsp.symlink(hop, path.join(watchedPath, 'a-link'));

      await watchWhile(async (events, watch) => {
        await whileHeld(() => {
          fs.renameSync(original, blocked);
          fs.unlinkSync(hop);
          fs.symlinkSync(blocked, hop);
          fs.writeFileSync(path.join(blocked, 'deep', 'queued.file'), '');
        });
        await barrier(events);
        assert.deepStrictEqual(events.filter(event => event.file === 'queued.file'), []);
        assert.deepStrictEqual(await reportedIn(events, blocked, ['after-move.file']), []);

        await watch.updateExcludedPaths([]);
        assert.deepStrictEqual(await reportedIn(events, blocked, ['restored.file']), ['blocked/restored.file']);
      }, [blocked]);
    });

    for (const nested of [false, true]) {
      const arrivingEntry = nested ? 'the parent of an excluded path' : 'an excluded path';
      it(`drops a stale link to a physical descendant that arrives at ${arrivingEntry}`, async function() {
        const original = path.join(outsidePath, 'original');
        const incoming = path.join(watchedPath, 'incoming');
        const blocked = nested ? path.join(incoming, 'blocked') : incoming;
        const originalBlocked = nested ? path.join(original, 'blocked') : original;
        const kept = path.join(watchedPath, 'z-kept');
        await mkdirp(path.join(originalBlocked, 'inner', 'deep'));
        await mkdirp(kept);
        await fsp.symlink(path.join(originalBlocked, 'inner', 'deep'), path.join(watchedPath, 'a-link'));
        await fsp.symlink(kept, path.join(originalBlocked, 'inner', 'deep', 'kept-link'));

        await watchWhile(async events => {
          await whileHeld(() => {
            fs.renameSync(original, incoming);
            fs.writeFileSync(path.join(blocked, 'inner', 'deep', 'queued.file'), '');
            fs.writeFileSync(path.join(kept, 'kept.file'), '');
          });
          await barrier(events);
          assert.deepStrictEqual(events.filter(event => event.file === 'queued.file'), []);
          const keptCreations = events.filter(event => event.action === nsfw.actions.CREATED &&
            event.file === 'kept.file');
          assert.deepStrictEqual(keptCreations.map(event => event.directory), [kept]);
          assert.deepStrictEqual(await reportedIn(events, blocked, ['inner/deep/later.file']), []);
        }, [blocked]);
      });
    }

    // The move into the excluded directory is between places that aren't watched: the symlink repointed after it is
    // what tells where the directories went, also for the link that leads through it.
    it('drops the links that lead through a symlink repointed into an excluded directory', async function() {
      const blocked = path.join(watchedPath, 'blocked');
      const hop = path.join(watchedPath, 's-hop');
      await mkdirp(path.join(outsidePath, 'A', 'D'));
      await mkdirp(path.join(blocked, 'inner'));
      await fsp.symlink(path.join(hop, 'D'), path.join(watchedPath, 'a-link'));
      await fsp.symlink(path.join(outsidePath, 'A'), hop);

      await watchWhile(async events => {
        assert.deepStrictEqual(await reportedIn(events, path.join(outsidePath, 'A'), ['D/before.file', 'top.file']),
          ['a-link/before.file', 's-hop/top.file']);
        fs.renameSync(path.join(outsidePath, 'A'), path.join(blocked, 'A'));
        fs.symlinkSync(path.join(blocked, 'A'), hop + '.new');
        fs.renameSync(hop + '.new', hop);
        assert.deepStrictEqual(await reportedIn(events, path.join(blocked, 'A'), ['D/later.file', 'later-top.file']),
          []);
      }, [blocked]);
    });

    it('watches a skipped link once a symlink it leads through is repointed out of the excluded directory',
      async function() {
        const blocked = path.join(watchedPath, 'blocked');
        const hop = path.join(watchedPath, 's-hop');
        await mkdirp(path.join(blocked, 'q'));
        await mkdirp(path.join(outsidePath, 'A'));
        await mkdirp(path.join(outsidePath, 'B', 'sub'));
        await fsp.symlink(path.join(blocked, 'q'), path.join(outsidePath, 'A', 'sub'));
        await fsp.symlink(path.join(hop, 'sub'), path.join(watchedPath, 'a-link'));
        await fsp.symlink(path.join(blocked, 'q'), path.join(watchedPath, 'b-direct'));
        await fsp.symlink(path.join(outsidePath, 'A'), hop);

        await watchWhile(async events => {
          assert.deepStrictEqual(await reportedIn(events, path.join(blocked, 'q'), ['hidden.file']), []);
          fs.symlinkSync(path.join(outsidePath, 'B'), hop + '.new');
          fs.renameSync(hop + '.new', hop);
          assert.deepStrictEqual(await reportedIn(events, path.join(outsidePath, 'B', 'sub'), ['restored.file']),
            ['a-link/restored.file']);

          // Removed and made again, in two steps, it is looked up again as well.
          const direct = path.join(watchedPath, 'b-direct');
          fs.unlinkSync(direct);
          await barrier(events);
          fs.symlinkSync(path.join(outsidePath, 'B'), direct);
          assert.deepStrictEqual(await reportedIn(events, path.join(outsidePath, 'B'), ['again.file']),
            ['b-direct/again.file']);
        }, [blocked]);
      });

    // Every excluded directory is read once for a directory held at a dangling link, when a directory leaves for a
    // place that isn't watched. After that, only the excluded path something arrives at is read for it again.
    it('drops a stale link looked for before once its directory arrives at an excluded path', async function() {
      const top = path.join(outsidePath, 'top');
      const blocked = path.join(watchedPath, 'blocked');
      const other = path.join(watchedPath, 'z-other');
      const leaving = path.join(watchedPath, 'leaving');
      await mkdirp(path.join(top, 'first', 'deep'));
      await mkdirp(path.join(other, 'inner'));
      await mkdirp(leaving);
      await fsp.symlink(path.join(top, 'first', 'deep'), path.join(watchedPath, 'a-link'));

      await watchWhile(async events => {
        assert.deepStrictEqual(await reportedIn(events, path.join(top, 'first', 'deep'), ['before.file']),
          ['a-link/before.file']);
        // No name the link led by is left to look its directory up with.
        fs.renameSync(path.join(top, 'first'), path.join(top, 'second'));
        fs.renameSync(leaving, path.join(outsidePath, 'left'));
        await waitFor(events, event => event.action === nsfw.actions.DELETED && event.file === 'leaving',
          'deletion of the directory that left');
        await barrier(events);
        await whileHeld(() => {
          fs.renameSync(top, blocked);
          fs.writeFileSync(path.join(blocked, 'second', 'deep', 'queued.file'), '');
        });
        await barrier(events);
        assert.deepStrictEqual(events.filter(event => event.file === 'queued.file'), []);
        assert.deepStrictEqual(await reportedIn(events, blocked, ['second/deep/later.file']), []);
      }, [blocked, other]);
    });

    // Searching the arriving directory is enough to hold watches below it, but listing it isn't possible.
    for (const [mode, restricted] of [[0o300, 'before'], [0o111, 'after']]) {
      it(`drops a stale link into an excluded directory made unreadable ${restricted} it arrives`, async function(t) {
        if (process.getuid() === 0) {
          return t.skip();
        }
        const original = path.join(outsidePath, 'original');
        const blocked = path.join(watchedPath, 'blocked');
        await mkdirp(path.join(original, 'inner', 'deep'));
        await fsp.symlink(path.join(original, 'inner', 'deep'), path.join(watchedPath, 'a-link'));

        try {
          await watchWhile(async events => {
            await whileHeld(() => {
              if (restricted === 'before') fs.chmodSync(original, mode);
              fs.renameSync(original, blocked);
              if (restricted === 'after') fs.chmodSync(blocked, mode);
              fs.writeFileSync(path.join(blocked, 'inner', 'deep', 'queued.file'), '');
            });
            await barrier(events);
            assert.deepStrictEqual(events.filter(event => event.file === 'queued.file'), []);
            assert.deepStrictEqual(await reportedIn(events, blocked, ['inner/deep/later.file']), []);
          }, [blocked]);
        } finally {
          for (const directory of [original, blocked]) {
            if (fs.existsSync(directory)) fs.chmodSync(directory, 0o700);
          }
        }
      });
    }

    it('drops a stale link into an unreadable directory that an update excludes after it arrived', async function(t) {
      if (process.getuid() === 0) {
        return t.skip();
      }
      const original = path.join(outsidePath, 'original');
      const blocked = path.join(watchedPath, 'blocked');
      await mkdirp(path.join(original, 'inner', 'deep'));
      await fsp.symlink(path.join(original, 'inner', 'deep'), path.join(watchedPath, 'a-link'));

      try {
        await watchWhile(async (events, watch) => {
          await whileHeld(() => {
            fs.chmodSync(original, 0o300);
            fs.renameSync(original, blocked);
          });
          await barrier(events);
          await watch.updateExcludedPaths([blocked]);
          assert.deepStrictEqual(await reportedIn(events, blocked, ['inner/deep/later.file']), []);
        });
      } finally {
        for (const directory of [original, blocked]) {
          if (fs.existsSync(directory)) fs.chmodSync(directory, 0o700);
        }
      }
    });

    for (const startsExcluded of [true, false]) {
      const destinationParent = startsExcluded ? 'an unwatched parent' : 'a recently excluded parent';
      it(`drops a link that still leads to a directory moved under ${destinationParent}`, async function() {
        const original = path.join(watchedPath, 'original');
        const blocked = path.join(watchedPath, 'blocked');
        const destination = path.join(blocked, 'original');
        const hop = path.join(outsidePath, 'hop');
        await mkdirp(path.join(original, 'deep'));
        await mkdirp(blocked);
        await fsp.symlink(path.join(original, 'deep'), hop);
        await fsp.symlink(hop, path.join(watchedPath, 'a-link'));

        await watchWhile(async (events, watch) => {
          const move = () => {
            fs.renameSync(original, destination);
            fs.unlinkSync(hop);
            fs.symlinkSync(path.join(destination, 'deep'), hop);
            fs.writeFileSync(path.join(destination, 'deep', 'queued.file'), '');
          };
          if (startsExcluded) {
            await whileHeld(move);
          } else {
            await untilLoopWaits('nsfw-test-pause-after-read', 'nsfw-test-resume-event-loop');
            try {
              // Leave the destination's dropped watch in the kernel so it still queues MOVED_TO.
              touch('before.file');
              await watch.updateExcludedPaths([blocked]);
              move();
            } finally {
              touch('nsfw-test-resume-event-loop');
            }
          }
          await barrier(events);
          assert.deepStrictEqual(events.filter(event => event.file === 'queued.file'), []);
          assert.deepStrictEqual(await reportedIn(events, destination, ['deep/later.file']), []);
        }, startsExcluded ? [blocked] : []);
      });
    }

    it('checks the current target when an exclusion goes through a repointed link', async function() {
      const first = path.join(watchedPath, 'first', 'blocked');
      const second = path.join(watchedPath, 'second', 'blocked');
      const route = path.join(watchedPath, 'route');
      await mkdirp(first);
      await mkdirp(second);
      await fsp.symlink('first', route);
      await fsp.symlink(first, path.join(watchedPath, 'a-first'));
      await fsp.symlink(second, path.join(watchedPath, 'b-second'));

      await watchWhile(async events => {
        assert.deepStrictEqual(await reportedIn(events, first, ['initial.file']), []);
        assert.deepStrictEqual(await reportedIn(events, second, ['visible.file']), ['b-second/visible.file']);
        await whileHeld(() => {
          fs.symlinkSync('second', route + '-replacement');
          fs.renameSync(route + '-replacement', route);
          fs.writeFileSync(path.join(second, 'queued.file'), '');
        });
        await barrier(events);
        assert.deepStrictEqual(events.filter(event => event.file === 'queued.file'), []);
        assert.deepStrictEqual(await reportedIn(events, second, ['now-blocked.file']), []);
        assert.deepStrictEqual(await reportedIn(events, first, ['unblocked.file']), ['first/blocked/unblocked.file']);
      }, [path.join(route, 'blocked')]);
    });
  });

  async function waitForRegistration(signal, attempt) {
    for (let i = 0; i < 500 && fs.statSync(signal).size < attempt; ++i) await sleep(10);
    assert.strictEqual(fs.statSync(signal).size, attempt);
  }

  it('finishes re-including a link whose external directory changes during registration', async function() {
    const link = path.join(watchedPath, 'nsfw-test-changing-link');
    const target = path.join(outsidePath, 'target');
    const signal = watchedPath + '.acquiring';
    await mkdirp(watchedPath);
    await mkdirp(target);
    await fsp.symlink(target, link);
    await fsp.writeFile(signal, '');
    await watchWhile(async (events, watch) => {
      const update = watch.updateExcludedPaths([]);
      for (let attempt = 1; attempt <= 2; ++attempt) {
        await waitForRegistration(signal, attempt);
        fs.renameSync(target, target + attempt);
        fs.mkdirSync(target);
      }
      await update;
      await fsp.writeFile(path.join(target, 'after-update'), '');
      await waitFor(events, event => event.action === nsfw.actions.CREATED && event.directory === link &&
        event.file === 'after-update', 'creation in replacement target after update');
    }, [link]);
  });

  it('keeps an owned watch and acquires the external target visible after registration', async function() {
    const link = path.join(watchedPath, 'nsfw-test-changing-link');
    const owned = path.join(watchedPath, 'owned');
    const target = path.join(outsidePath, 'target');
    await mkdirp(owned);
    await mkdirp(target);
    await fsp.symlink(target, link);
    const acquiring = watchedPath + '.acquiring';
    const registered = watchedPath + '.registered';
    await fsp.writeFile(acquiring, '');
    await fsp.writeFile(registered, '');
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
        await fsp.writeFile(path.join(directory, 'after-update'), '');
        await waitFor(events, event => event.action === nsfw.actions.CREATED && event.directory === directory &&
          event.file === 'after-update', 'creation in ' + directory);
      }
    }, [link]);
  });

  it('watches a directory that left at a symlink to it created in the same read, with the exclusions there',
    async function() {
      await mkdirp(path.join(watchedPath, 'real', 'inner'));
      const linkPath = path.join(watchedPath, 'links', 'link');

      await watchWhile(async events => {
        await whileHeld(async () => {
          await mkdirp(path.join(watchedPath, 'links'));
          await fsp.rename(path.join(watchedPath, 'real'), path.join(outsidePath, 'moved'));
          await fsp.symlink(path.join(outsidePath, 'moved'), linkPath);
        });
        assert.deepStrictEqual(
          await reportedIn(events, path.join(outsidePath, 'moved'), ['top.file', 'inner/deep.file']),
          [path.join('links', 'link', 'top.file')]
        );
      }, [path.join(linkPath, 'inner')]);
    });

  describe('with exclusions spelled by another path than the symlink the watches are under', function() {
    // Listed before real, which it leads to: the tree has real's directories under link.
    const realPath = path.join(watchedPath, 'real');
    const reported = (...names) => path.join('link', ...names);

    beforeEach(async function() {
      await mkdirp(realPath);
      await fsp.symlink('real', path.join(watchedPath, 'link'));
    });

    it('watches a directory excluded where a move takes its parent once a later move in the same read takes it on',
      async function() {
        await mkdirp(path.join(realPath, 'source', 'blocked'));

        await watchWhile(async events => {
          await whileHeld(async () => {
            // When the first move is handled, target, where blocked is excluded, is gone already.
            await fsp.rename(path.join(realPath, 'source'), path.join(realPath, 'target'));
            await fsp.rename(path.join(realPath, 'target'), path.join(realPath, 'moved'));
          });
          assert.deepStrictEqual(
            await reportedIn(events, path.join(realPath, 'moved'), ['top.file', 'blocked/deep.file']),
            [reported('moved', 'top.file'), reported('moved', 'blocked', 'deep.file')]
          );
        }, [path.join(realPath, 'target', 'blocked')]);
      });

    it('does not watch a directory excluded where the last move of its parent in a read takes it', async function() {
      await mkdirp(path.join(realPath, 'source', 'blocked'));

      await watchWhile(async events => {
        await whileHeld(async () => {
          // When the first move is handled, the excluded path leads to blocked already, below a parent the tree has
          // at another name.
          await fsp.rename(path.join(realPath, 'source'), path.join(realPath, 'between'));
          await fsp.rename(path.join(realPath, 'between'), path.join(realPath, 'target'));
        });
        assert.deepStrictEqual(
          await reportedIn(events, path.join(realPath, 'target'), ['top.file', 'blocked/deep.file']),
          [reported('target', 'top.file')]
        );
      }, [path.join(realPath, 'target', 'blocked')]);
    });

    it('watches a directory excluded under the name its parent leaves, once another directory has that name',
      async function() {
        await mkdirp(path.join(realPath, 'parent', 'blocked'));

        await watchWhile(async events => {
          await whileHeld(async () => {
            // When the move is handled, the excluded path leads into the new parent, which has no node yet.
            await fsp.rename(path.join(realPath, 'parent'), path.join(realPath, 'moved'));
            await mkdirp(path.join(realPath, 'parent', 'blocked'));
          });
          assert.deepStrictEqual(
            await reportedIn(events, path.join(realPath, 'moved'), ['top.file', 'blocked/deep.file']),
            [reported('moved', 'top.file'), reported('moved', 'blocked', 'deep.file')]
          );
          assert.deepStrictEqual(
            await reportedIn(events, path.join(realPath, 'parent'), ['new.file', 'blocked/hidden.file']),
            [reported('parent', 'new.file')]
          );
        }, [path.join(realPath, 'parent', 'blocked')]);
      });

    it('reports what was created in a directory once a later move in the same read includes it again',
      async function() {
        await mkdirp(path.join(realPath, 'source', 'blocked'));

        await watchWhile(async events => {
          await whileHeld(async () => {
            // The first move excludes blocked and drops its watch, which has this creation queued.
            await fsp.rename(path.join(realPath, 'source'), path.join(realPath, 'target'));
            await fsp.rename(path.join(realPath, 'target'), path.join(realPath, 'moved'));
            await fsp.writeFile(path.join(realPath, 'moved', 'blocked', 'inside.file'), 'Inside');
          });
          await waitFor(events, event => event.action === nsfw.actions.CREATED &&
            event.directory === path.join(watchedPath, 'link', 'moved', 'blocked') && event.file === 'inside.file',
          'creation in the directory included again');
        }, [path.join(realPath, 'target', 'blocked')]);
      });

    it('excludes a directory below one replaced while the first scan runs', async function() {
      for (const name of ['a/hidden', 'b', 'c', 'd', 'x/hidden']) {
        await mkdirp(path.join(realPath, name));
      }
      const events = [];
      const watch = await nsfw(watchedPath, batch => events.push(...batch), {
        debounceMS: 100, excludedPaths: [path.join(realPath, 'x', 'hidden')]
      });
      const otherWatches = countInotifyWatches();
      try {
        const started = watch.start();
        // Once the watched path, link, a and a/hidden are watched, the scan has asked about a/hidden, named like the
        // excluded directory, and the slow build takes 500 ms for each of b, c and d before it reaches x.
        for (let i = 0; i < 1000 && countInotifyWatches() < otherWatches + 4; ++i) {
          await sleep(10);
        }
        assert.ok(countInotifyWatches() >= otherWatches + 4, 'The first scan never watched a/hidden');
        await remove(path.join(realPath, 'x'));
        await mkdirp(path.join(realPath, 'x', 'hidden'));
        await started;
        assert.deepStrictEqual(
          await reportedIn(events, path.join(realPath, 'x'), ['top.file', 'hidden/deep.file']),
          [reported('x', 'top.file')]
        );
      } finally {
        touch('nsfw-test-release-event-loop');
        await watch.stop();
      }
    });

    it('does not report what was created in a directory while a move in the same read had it at an excluded path',
      async function() {
        await mkdirp(path.join(realPath, 'source', 'blocked'));
        await mkdirp(path.join(realPath, 'other'));
        const excludedPath = path.join(realPath, 'target', 'blocked');

        await watchWhile(async events => {
          await whileHeld(async () => {
            await fsp.rename(path.join(realPath, 'source'), path.join(realPath, 'target'));
            await fsp.writeFile(path.join(excludedPath, 'inside.file'), 'Inside');
            await fsp.rename(path.join(realPath, 'target'), path.join(realPath, 'moved'));
            // When the first move is handled, the excluded path's parent is this directory on disk.
            await fsp.rename(path.join(realPath, 'other'), path.join(realPath, 'target'));
          });
          await waitFor(events, event => event.action === nsfw.actions.CREATED &&
            event.directory === path.join(watchedPath, 'link', 'moved', 'blocked') && event.file === 'inside.file',
          'creation in the directory included again');
          assert.deepStrictEqual(
            events.filter(event => event.directory === path.join(watchedPath, 'link', 'target', 'blocked')), []);
        }, [excludedPath]);
      });

    it('watches a directory when its parent leaves its inode number to one on an excluded path', async function(t) {
      const heldPath = path.join(realPath, 'd', 'src', 'x');
      await outputFile(path.join(heldPath, 'held.file'), 'Held');
      const inode = fs.statSync(path.join(realPath, 'd')).ino;
      let reused = false;

      await watchWhile(async events => {
        await whileHeld(async () => {
          fs.renameSync(path.join(realPath, 'd', 'src'), path.join(realPath, 'd', 'blocked'));
          fs.rmSync(path.join(realPath, 'd'), { recursive: true, force: true });
          fs.mkdirSync(path.join(realPath, 'q'));
          reused = fs.statSync(path.join(realPath, 'q')).ino === inode;
        });
        if (!reused) {
          // Only what the file system chose shows the case.
          return t.skip();
        }
        // x was never q/blocked/x: what it held is reported as it goes.
        await waitFor(events, event => event.action === nsfw.actions.DELETED &&
          event.directory === path.join(watchedPath, 'link', 'd', 'blocked', 'x') && event.file === 'held.file',
        'deletion in x');
      }, [path.join(realPath, 'q', 'blocked', 'x')]);
    });

    it('watches what an excluded path stops naming when the rename of its directory is read after its symlink went',
      async function() {
        const renamedPath = path.join(watchedPath, 'renamed');
        await mkdirp(path.join(realPath, 'sub', 'deep'));

        await watchWhile(async events => {
          // The removal of the symlink is read with the rename already done, and handled before the rename is read:
          // by then nothing can be watched below the symlink, nor is the directory where the tree saw it.
          await untilLoopWaits('nsfw-test-hold-event-loop', 'nsfw-test-release-event-loop');
          await remove(waitingPath);
          fs.unlinkSync(path.join(watchedPath, 'link'));
          touch('nsfw-test-pause-after-read');
          touch('nsfw-test-release-event-loop');
          await untilWaiting('The event loop never waited before the lock');
          fs.renameSync(realPath, renamedPath);
          touch('nsfw-test-resume-event-loop');

          const files = ['top.file', path.join('sub', 'inner.file'), path.join('sub', 'deep', 'deeper.file')];
          assert.deepStrictEqual(
            (await reportedIn(events, renamedPath, files)).sort(),
            files.map(file => path.join('renamed', file)).sort()
          );
        }, [path.join(realPath, 'sub')]);
      });

    it('watches a directory moved to an excluded path once a later move in the same read takes it on',
      async function() {
        await mkdirp(path.join(realPath, 'visible', 'inner'));

        await watchWhile(async events => {
          await whileHeld(async () => {
            await fsp.rename(path.join(realPath, 'visible'), path.join(realPath, 'hidden'));
            await fsp.rename(path.join(realPath, 'hidden'), path.join(realPath, 'shown'));
          });
          assert.deepStrictEqual(
            await reportedIn(events, path.join(realPath, 'shown'), ['top.file', 'inner/deep.file']),
            [reported('shown', 'top.file'), reported('shown', 'inner', 'deep.file')]
          );
        }, [path.join(realPath, 'hidden')]);
      });
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
      await outputFile(path.join(ownPath, 'sub', 'held.file'), 'Held');
      await mkdirp(holderPath);
      await fsp.symlink(path.join('..', 'packages', 'pkg'), path.join(holderPath, 'pkg'));

      await watchWhile(async (events, watch) => {
        // The read is held without the lock, so the update goes first.
        await untilLoopWaits('nsfw-test-pause-after-read', 'nsfw-test-resume-event-loop');
        await remove(path.join(ownPath, 'sub', 'held.file'));
        await fsp.writeFile(path.join(ownPath, 'written.file'), 'Written');
        await watch.updateExcludedPaths([holderPath]);
        touch('nsfw-test-resume-event-loop');
        await expectReportedAtOwnPath(events, ownPath, holderPath);
      });
    });

    it('reports at its own path what the read of a rename that left the symlink dangling holds', async function() {
      const linkPath = path.join(watchedPath, 'a-link');
      const ownPath = path.join(watchedPath, 'moved', 'real');
      await outputFile(path.join(watchedPath, 'deep', 'real', 'sub', 'held.file'), 'Held');
      await fsp.symlink(path.join('deep', 'real'), linkPath);

      await watchWhile(async events => {
        await whileHeld(async () => {
          await fsp.rename(path.join(watchedPath, 'deep'), path.join(watchedPath, 'moved'));
          await remove(path.join(ownPath, 'sub', 'held.file'));
          await fsp.writeFile(path.join(ownPath, 'written.file'), 'Written');
        });
        await expectReportedAtOwnPath(events, ownPath, linkPath);
      });
    });
  });

  for (const [description, operation] of [
    ['before', async () => {
      await fsp.rename(path.join(watchedPath, 'p', 'X'), path.join(watchedPath, 'A', 'X'));
      await fsp.rename(path.join(watchedPath, 'p'), path.join(outsidePath, 'p'));
    }],
    ['after', async () => {
      await fsp.rename(path.join(watchedPath, 'p'), path.join(outsidePath, 'p'));
      await fsp.rename(path.join(outsidePath, 'p', 'X'), path.join(watchedPath, 'A', 'X'));
    }]
  ]) {
    it(`watches a directory moved into a directory created in the same read, ${description} its old parent leaves`,
      async function() {
        await mkdirp(path.join(watchedPath, 'p', 'X', 'inner'));

        await watchWhile(async events => {
          await whileHeld(async () => {
            await mkdirp(path.join(watchedPath, 'A'));
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
      await mkdirp(path.join(watchedPath, 'a'));
      await mkdirp(path.join(outsidePath, 'P', 'z'));

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
      await mkdirp(path.join(watchedPath, 'a'));
      await mkdirp(path.join(outsidePath, 'P', 'z'));
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
    await mkdirp(path.join(watchedPath, 'a'));
    await mkdirp(path.join(outsidePath, 'P', 'z'));

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
      await mkdirp(path.join(watchedPath, 'a'));
      await mkdirp(path.join(watchedPath, 'h'));
      await mkdirp(path.join(outsidePath, 'P', 'z'));
      const hop = path.join(outsidePath, 'hop');
      const aliasPath = path.join(watchedPath, 'P', 'z', 'alias');

      await watchWhile(async events => {
        await whileHeld(async () => {
          fs.renameSync(path.join(watchedPath, 'a'), path.join(outsidePath, 'P', 'a'));
          fs.renameSync(path.join(outsidePath, 'P'), path.join(watchedPath, 'P'));
          await remove(waitingPath);
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
        await untilWaiting('The event loop never waited in h');
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
    await mkdirp(path.join(watchedPath, 'X', 'inner'));
    await mkdirp(path.join(watchedPath, 'excluded'));

    await watchWhile(async events => {
      await whileScanning(events, () => fsp.rename(path.join(watchedPath, 'archive', 'x'),
        path.join(watchedPath, 'excluded', 'x')));
      assert.deepStrictEqual(
        await reportedIn(events, path.join(watchedPath, 'excluded', 'x'), ['top.file', 'inner/deep.file']), []);
    }, [path.join(watchedPath, 'excluded')]);
  });

  it('does not watch a directory seen by a scan of one that then left the watched path', async function() {
    await mkdirp(path.join(watchedPath, 'X', 'inner'));

    await watchWhile(async events => {
      await whileScanning(events, () => fsp.rename(path.join(watchedPath, 'archive'),
        path.join(outsidePath, 'archive')));
      assert.deepStrictEqual(
        await reportedIn(events, path.join(outsidePath, 'archive', 'x'), ['top.file', 'inner/deep.file']), []);
    });
  });

  it('watches a directory that got the inode number of one deleted in the same read', async function(t) {
    await mkdirp(path.join(watchedPath, 'old'));
    const oldInode = fs.statSync(path.join(watchedPath, 'old')).ino;
    let reused = false;

    await watchWhile(async events => {
      await whileHeld(async () => {
        await mkdirp(path.join(watchedPath, 'S'));
        fs.rmdirSync(path.join(watchedPath, 'old'));
        fs.mkdirSync(path.join(watchedPath, 'S', 'new'));
        reused = fs.statSync(path.join(watchedPath, 'S', 'new')).ino === oldInode;
      });
      if (!reused) {
        // Only what the file system chose shows the case.
        return t.skip();
      }
      assert.deepStrictEqual(await reportedIn(events, path.join(watchedPath, 'S', 'new'), ['top.file']),
        [path.join('S', 'new', 'top.file')]);
    });
  });

  it('watches a directory that got the inode number of one that left and was deleted in the same read',
    async function(t) {
      await mkdirp(path.join(watchedPath, 'old'));
      await mkdirp(path.join(watchedPath, 'excluded'));
      const oldInode = fs.statSync(path.join(watchedPath, 'old')).ino;
      let reused = false;

      await watchWhile(async events => {
        await whileHeld(async () => {
          await mkdirp(path.join(watchedPath, 'S'));
          fs.renameSync(path.join(watchedPath, 'old'), path.join(watchedPath, 'excluded', 'old'));
          fs.writeFileSync(path.join(watchedPath, 'excluded', 'old', 'secret.file'), 'Secret');
          fs.rmSync(path.join(watchedPath, 'excluded', 'old'), { recursive: true, force: true });
          fs.mkdirSync(path.join(watchedPath, 'S', 'new'));
          reused = fs.statSync(path.join(watchedPath, 'S', 'new')).ino === oldInode;
        });
        if (!reused) {
          return t.skip();
        }
        // S's scan saw new with old's inode number, but new isn't old: old's events are its own, not new's.
        assert.deepStrictEqual(await reportedIn(events, path.join(watchedPath, 'S', 'new'), ['top.file']),
          [path.join('S', 'new', 'top.file')]);
        assert.deepStrictEqual(events.filter(event => event.file === 'secret.file'), []);
      }, [path.join(watchedPath, 'excluded')]);
    });

  it('does not report what an excluded directory queued once a scan watches it again in the same read',
    async function() {
      await mkdirp(path.join(watchedPath, 'd', 'sub'));
      await mkdirp(path.join(watchedPath, 'x'));
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
        await fsp.writeFile(path.join(watchedPath, 'e', 'sub', 'top.file'), 'Top');
        await waitFor(events, event => event.action === nsfw.actions.CREATED &&
          event.directory === path.join(watchedPath, 'e', 'sub') && event.file === 'top.file', 'creation in sub');
        assert.deepStrictEqual(events.filter(event => event.file === 'secret.file'), []);
      }, [path.join(watchedPath, 'x')]);
    });

  it('does not report what an excluded directory queued once the scan of another one moved to its new place finds it',
    async function() {
      await mkdirp(path.join(watchedPath, 'd'));
      await mkdirp(path.join(watchedPath, 'd2', 'sub'));
      await mkdirp(path.join(watchedPath, 'x'));
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
        await fsp.writeFile(path.join(watchedPath, 'e', 'sub', 'top.file'), 'Top');
        await waitFor(events, event => event.action === nsfw.actions.CREATED &&
          event.directory === path.join(watchedPath, 'e', 'sub') && event.file === 'top.file', 'creation in sub');
        assert.deepStrictEqual(events.filter(event => event.file === 'secret.file'), []);
      }, [path.join(watchedPath, 'x')]);
    });

  it('watches a directory moved out and back after a modification',
    async function() {
      await mkdirp(path.join(watchedPath, 'round', 'trip'));
      touch(path.join('round', 'trip', 'existing.file'));

      await watchWhile(async events => {
        await whileHeld(async () => {
          await fsp.rename(path.join(watchedPath, 'round'), path.join(outsidePath, 'round'));
          await fsp.rename(path.join(outsidePath, 'round'), path.join(watchedPath, 'round'));
        });
        const existingPath = path.join(watchedPath, 'round', 'trip', 'existing.file');
        await fsp.appendFile(existingPath, 'Modified');
        await checkReturnState(events, path.dirname(existingPath), 'existing.file', true);
      });
    });

  it('does not watch a directory that came back to a parent that left in the same read', async function() {
    await mkdirp(path.join(watchedPath, 'p', 'd'));

    await watchWhile(async events => {
      await whileHeld(async () => {
        await fsp.rename(path.join(watchedPath, 'p', 'd'), path.join(outsidePath, 'd'));
        await fsp.rename(path.join(outsidePath, 'd'), path.join(watchedPath, 'p', 'd'));
        await fsp.rename(path.join(watchedPath, 'p'), path.join(outsidePath, 'p'));
      });
      await barrier(events);
      await fsp.writeFile(path.join(outsidePath, 'p', 'd', 'outside.file'), 'Outside');
      await barrier(events);
      assert.deepStrictEqual(events.filter(event => event.file === 'outside.file'), []);
    });
  });

  it('watches by their events a directory that got the inode number of one deleted and the one taking its name',
    async function(t) {
      await mkdirp(path.join(watchedPath, 'old'));
      const oldInode = fs.statSync(path.join(watchedPath, 'old')).ino;
      let reused = false;

      await watchWhile(async events => {
        await whileHeld(async () => {
          await mkdirp(path.join(watchedPath, 'S'));
          fs.rmdirSync(path.join(watchedPath, 'old'));
          fs.mkdirSync(path.join(watchedPath, 'S', 'new'));
          reused = fs.statSync(path.join(watchedPath, 'S', 'new')).ino === oldInode;
          // Listed after new, so S's scan takes the slow build's 500 ms for each of them once it saw new.
          for (const name of ['z1', 'z2', 'z3']) {
            fs.mkdirSync(path.join(watchedPath, 'S', name));
          }
        });
        if (!reused) {
          return t.skip();
        }
        // Before the deletion of old is handled: new leaves its name to another directory.
        await waitFor(events, event => event.action === nsfw.actions.CREATED &&
          event.directory === path.join(watchedPath, 'S') && event.file === 'new', 'scan of S');
        await fsp.rename(path.join(watchedPath, 'S', 'new'), path.join(watchedPath, 'S', 'moved'));
        await mkdirp(path.join(watchedPath, 'S', 'new'));

        assert.deepStrictEqual(await reportedIn(events, path.join(watchedPath, 'S', 'new'), ['new.file']),
          [path.join('S', 'new', 'new.file')]);
        assert.deepStrictEqual(await reportedIn(events, path.join(watchedPath, 'S', 'moved'), ['moved.file']),
          [path.join('S', 'moved', 'moved.file')]);
      });
    });
});
