const { afterEach, beforeEach, describe } = require('node:test');
const assert = require('assert');
const cp = require('child_process');
const fs = require('fs');
const fsp = require('fs/promises');
const os = require('os');
const path = require('path');
const { isDeepStrictEqual, promisify } = require('util');

const {
  DEBOUNCE, WORKDIR: workDir, barrier, countInotifyWatches, it, mkdirp, outputFile, remove, sleep, untilNextSecond,
  waitForEvent, waitUntil, watchCountReaches
} = require('./common');

const nsfw = require('../dist');

const execFile = promisify(cp.execFile);

describe('Node Sentinel File Watcher', function() {

  assert.ok(typeof nsfw._native.NSFW_TEST_SLOW === 'undefined', 'NSFW should NOT be built in slow mode');
  assert.ok(typeof nsfw._native.NSFW_TEST_FAULTS === 'undefined', 'NSFW should NOT be built with NSFW_TEST_FAULTS');

  beforeEach(async function() {
    async function makeDir(identifier) {
      await fsp.mkdir(path.join(workDir, 'test' + identifier));
      await fsp.mkdir(path.join(workDir, 'test' + identifier, 'folder' + identifier));
      await fsp.writeFile(path.join(workDir, 'test' + identifier, 'testing' + identifier +'.file'), 'testing');
    }

    try {
      await remove(workDir);
    } catch (e) {/* we don't care about this failure */}

    await fsp.mkdir(workDir);
    const promises = [];
    for (let i = 0; i < 10; ++i) {
      promises.push(makeDir(i));
    }

    await Promise.all(promises);
  });

  afterEach(function() {
    return remove(workDir);
  });

  describe('Basic', function() {
    it('returns watchers that are nsfw instances with detachable methods', async function() {
      const directory = path.join(workDir, 'test0', 'folder0');
      for (const watchPath of [directory, path.join(workDir, 'test0', 'testing0.file')]) {
        const watcher = await nsfw(watchPath, () => {});
        assert.ok(watcher instanceof nsfw);
        const { start, pause, resume, stop } = watcher;
        await start();
        try {
          await pause();
          await resume();
        } finally {
          await stop();
        }
      }
    });

    it('can watch a single file', async function() {
      const file = 'testing1.file';
      const inPath = path.resolve(workDir, 'test1');
      const filePath = path.join(inPath, file);
      let changeEvents = 0;
      let createEvents = 0;
      let deleteEvents = 0;

      function findEvents(element) {
        if (
          element.action === nsfw.actions.MODIFIED &&
          element.directory === path.resolve(inPath) &&
          element.file === file
        ) {
          changeEvents++;
        } else if (
          element.action === nsfw.actions.CREATED &&
          element.directory === path.resolve(inPath) &&
          element.file === file
        ) {
          createEvents++;
        } else if (
          element.action === nsfw.actions.DELETED &&
          element.directory === path.resolve(inPath) &&
          element.file === file
        ) {
          deleteEvents++;
        }
      }

      let watch = await nsfw(
        filePath,
        events => events.forEach(findEvents),
        { debounceMS: 100 }
      );

      try {
        await watch.start();
        await untilNextSecond(filePath);
        await fsp.writeFile(filePath, 'Bean bag video games at noon.');
        await waitUntil(() => changeEvents > 0, { message: 'No modification' });
        await remove(filePath);
        await waitUntil(() => deleteEvents > 0, { message: 'No deletion' });
        await fsp.writeFile(filePath, 'His watch has ended.');
        await waitUntil(() => createEvents > 0, { message: 'No creation' });

        assert.equal(createEvents, 1);
        assert.equal(deleteEvents, 1);
      } finally {
        await watch.stop();
        watch = null;
      }
    });

    it('can listen for a create event', async function() {
      const file = 'another_test.file';
      const inPath = path.resolve(workDir, 'test2', 'folder2');
      let eventFound = false;

      function findEvent(element) {
        if (
          element.action === nsfw.actions.CREATED &&
          element.directory === path.resolve(inPath) &&
          element.file === file
        ) {
          eventFound = true;
        }
      }

      let watch = await nsfw(
        workDir,
        events => events.forEach(findEvent),
        { debounceMS: DEBOUNCE }
      );

      try {
        await watch.start();
        await fsp.writeFile(path.join(inPath, file), 'Peanuts, on occasion, rain from the skies.');
        await waitUntil(() => eventFound, { message: 'No event' });
      } finally {
        await watch.stop();
        watch = null;
      }
    });

    it('can listen for a delete event', async function() {
      const file = 'testing3.file';
      const inPath = path.resolve(workDir, 'test3');
      let eventFound = false;

      function findEvent(element) {
        if (
          element.action === nsfw.actions.DELETED &&
          element.directory === path.resolve(inPath) &&
          element.file === file
        ) {
          eventFound = true;
        }
      }

      let watch = await nsfw(
        workDir,
        events => events.forEach(findEvent),
        { debounceMS: DEBOUNCE }
      );

      try {
        await watch.start();
        await remove(path.join(inPath, file));
        await waitUntil(() => eventFound, { message: 'No event' });
      } finally {
        await watch.stop();
        watch = null;
      }
    });

    it('can listen for a modify event', async function() {
      const file = 'testing0.file';
      const inPath = path.resolve(workDir, 'test0');
      let eventFound = false;

      function findEvent(element) {
        if (
          element.action === nsfw.actions.MODIFIED &&
          element.directory === path.resolve(inPath) &&
          element.file === file
        ) {
          eventFound = true;
        }
      }

      let watch = await nsfw(
        workDir,
        events => events.forEach(findEvent),
        { debounceMS: DEBOUNCE }
      );

      try {
        await watch.start();
        await untilNextSecond(path.join(inPath, file));
        await fsp.writeFile(path.join(inPath, file), 'At times, sunflower seeds are all that is life.');
        await waitUntil(() => eventFound, { message: 'No event' });
      } finally {
        await watch.stop();
        watch = null;
      }
    });

    it('reports a file created and modified within the same second as created once', async function() {
      const { CREATED, MODIFIED } = nsfw.actions;
      const inPath = path.resolve(workDir, 'test5');
      const events = [];
      const named = (action, file) => events.filter(event =>
        event.action === action && event.directory === inPath && event.file === file);

      let watch = await nsfw(workDir, batch => events.push(...batch), { debounceMS: DEBOUNCE });

      try {
        await watch.start();
        // Both writes have to fall within one second, which a slow runner can miss: tried again from the start of
        // the next one until they do.
        let sameSecond = false;
        for (let attempt = 0; attempt < 5 && !sameSecond; attempt++) {
          const file = `created-then-modified-${attempt}.file`;
          await sleep(1000 - Date.now() % 1000 + 10);
          const writtenAt = Date.now();
          await fsp.writeFile(path.join(inPath, file), 'Created');
          await waitUntil(() => named(CREATED, file).length > 0,
            { message: () => `No creation in ${JSON.stringify(events)}` });
          await fsp.appendFile(path.join(inPath, file), ' and modified');
          sameSecond = Math.floor(Date.now() / 1000) === Math.floor(writtenAt / 1000);
          await waitUntil(() => named(MODIFIED, file).length > 0,
            { message: () => `No modification in ${JSON.stringify(events)}` });
          await barrier(events, workDir);
          assert.equal(named(CREATED, file).length, 1, JSON.stringify(events));
        }
        assert.ok(sameSecond, 'The two writes never fell within one second');
      } finally {
        await watch.stop();
        watch = null;
      }
    });

    it('reports a file created and removed at once as created before removed', async function() {
      const { CREATED, DELETED } = nsfw.actions;
      const inPath = path.resolve(workDir, 'test8');
      const file = 'short-lived.file';
      const events = [];

      let watch = await nsfw(workDir, batch => events.push(...batch), { debounceMS: DEBOUNCE });

      try {
        await watch.start();
        await fsp.writeFile(path.join(inPath, file), 'Short-lived');
        await fsp.unlink(path.join(inPath, file));
        await barrier(events, workDir);
        const actions = events.filter(event => event.directory === inPath && event.file === file)
          .map(event => event.action);
        if (process.platform === 'darwin') {
          assert.deepStrictEqual(actions, [CREATED, DELETED], JSON.stringify(events));
        } else {
          // Some watchers see nothing of a file gone before they look.
          const appearedAndLeft = actions.filter(action => action === CREATED || action === DELETED);
          assert.ok(appearedAndLeft.length === 0 || isDeepStrictEqual(appearedAndLeft, [CREATED, DELETED]),
            JSON.stringify(events));
        }
      } finally {
        await watch.stop();
        watch = null;
      }
    });

    it('reports renames in a row as renames that lead to the last name', async function() {
      const { CREATED, DELETED, RENAMED } = nsfw.actions;
      const inPath = path.resolve(workDir, 'test9');
      const events = [];
      // Replays the events about the names in inPath onto the ones there before, failing on one that doesn't fit.
      const replay = (names, start) => {
        const present = new Set(start);
        for (const event of events.filter(event => event.directory === inPath && names.some(name =>
          name === event.file || name === event.oldFile || name === event.newFile))) {
          assert.ok(![CREATED, DELETED].includes(event.action), JSON.stringify(events));
          if (event.action === RENAMED) {
            assert.ok(present.delete(event.oldFile) && event.newDirectory === inPath, JSON.stringify(events));
            present.add(event.newFile);
          }
        }
        return [...present].sort();
      };

      let watch = await nsfw(workDir, batch => events.push(...batch), { debounceMS: DEBOUNCE });

      try {
        await watch.start();
        const [a, b, c, home, away] = ['chain-a.file', 'chain-b.file', 'chain-c.file', 'home.file', 'away.file']
          .map(name => path.join(inPath, name));
        // Created while watching: FSEvents may coalesce a round trip into one event per name, which only tells where
        // the entry ends up, so where it started is where it was reported.
        await fsp.writeFile(a, 'Chain');
        await fsp.writeFile(home, 'Round trip');
        await barrier(events, workDir);
        const before = events.length;
        await fsp.rename(a, b);
        await fsp.rename(b, c);
        await fsp.rename(home, away);
        await fsp.rename(away, home);
        await barrier(events, workDir);
        events.splice(0, before);
        assert.deepStrictEqual(replay(['chain-a.file', 'chain-b.file', 'chain-c.file'], ['chain-a.file']),
          ['chain-c.file']);
        assert.deepStrictEqual(replay(['home.file', 'away.file'], ['home.file']), ['home.file']);
      } finally {
        await watch.stop();
        watch = null;
      }
    });

    it('keeps both names when two files swap through a temporary name', async function() {
      const { CREATED, DELETED, RENAMED } = nsfw.actions;
      const inPath = path.resolve(workDir, 'test9');
      const names = ['swap-a.file', 'swap-b.file', 'swap-tmp.file'];
      const [a, b, temp] = names.map(name => path.join(inPath, name));
      const events = [];
      let watch = await nsfw(inPath, batch => events.push(...batch), { debounceMS: DEBOUNCE });

      try {
        await watch.start();
        await fsp.writeFile(a, 'A');
        await fsp.writeFile(b, 'B');
        await barrier(events, inPath);
        const beforeSwap = events.length;
        fs.renameSync(a, temp);
        fs.renameSync(b, a);
        fs.renameSync(temp, b);
        await barrier(events, inPath);

        const swapEvents = events.slice(beforeSwap).filter(event => event.directory === inPath &&
          names.includes(event.action === RENAMED ? event.oldFile : event.file));
        const present = new Set(names.slice(0, 2));
        for (const event of swapEvents) {
          if (event.action === RENAMED) {
            assert.ok(present.delete(event.oldFile), JSON.stringify(events));
            assert.equal(event.newDirectory, inPath);
            present.add(event.newFile);
          } else if (event.action === CREATED) {
            present.add(event.file);
          } else if (event.action === DELETED) {
            present.delete(event.file);
          }
        }
        assert.deepStrictEqual([...present].sort(), names.slice(0, 2), JSON.stringify(events));
        assert.ok(swapEvents.some(event => event.action === RENAMED || event.action === DELETED));
      } finally {
        await watch.stop();
        watch = null;
      }
    });

    it('can listen for a rename event', async function() {
      const srcFile = 'testing.file';
      const destFile = 'new-testing.file';
      const inPath = path.resolve(workDir, 'test4');
      let eventListening = false;
      let deleteEventFound = false;
      let createEventFound = false;
      let renameEventFound = false;
      let extraEventFound = false;

      function findEvent(element) {
        if (!eventListening) {
          return;
        }
        if (
          element.action === nsfw.actions.RENAMED &&
          element.directory === inPath &&
          element.oldFile === srcFile &&
          element.newDirectory === inPath &&
          element.newFile === destFile
        ) {
          renameEventFound = true;
        } else if (
          element.action === nsfw.actions.DELETED &&
          element.directory === path.resolve(inPath) &&
          element.file === srcFile
        ) {
          deleteEventFound = true;
        } else if (
          element.action === nsfw.actions.CREATED &&
          element.directory === path.resolve(inPath) &&
          element.file === destFile
        ) {
          createEventFound = true;
        } else {
          if (element.directory === path.resolve(inPath)) {
            extraEventFound = true;
          }
        }
      }

      const events = [];
      let watch = await nsfw(
        workDir,
        batch => {
          batch.forEach(findEvent);
          events.push(...batch);
        },
        { debounceMS: DEBOUNCE }
      );

      try {
        await watch.start();
        await outputFile(path.join(inPath, srcFile), '');
        await barrier(events, workDir);
        eventListening = true;
        await fsp.rename(path.join(inPath, srcFile), path.join(inPath, destFile));
        await waitUntil(() => renameEventFound || (deleteEventFound && createEventFound),
          { message: () => `No rename in ${JSON.stringify(events)}` });
        await barrier(events, workDir);
        eventListening = false;

        assert.ok(renameEventFound, JSON.stringify(events));
        assert.ok(!deleteEventFound && !createEventFound, JSON.stringify(events));

        assert.ok(!extraEventFound);
      } finally {
        await watch.stop();
        watch = null;
      }
    });

    // A burst of renames fills reads up, so that the old name of some ends a read and their new name starts the next.
    it('reports each rename of a burst as one rename', async function(t) {
      if (process.platform === 'darwin') return t.skip();
      const inPath = path.join(workDir, 'test4', 'burst');
      const count = 2000;
      const oldName = i => `old-name-of-a-file-renamed-in-a-burst-${i}.file`;
      const newName = i => `new-name-of-a-file-renamed-in-a-burst-${i}.file`;
      await fsp.mkdir(inPath);
      for (let i = 0; i < count; i++) {
        await fsp.writeFile(path.join(inPath, oldName(i)), '');
      }

      const events = [];
      let watch = await nsfw(workDir, batch => events.push(...batch), { debounceMS: DEBOUNCE });
      try {
        await watch.start();
        for (let i = 0; i < count; i += 100) {
          const renames = [];
          for (let j = i; j < Math.min(i + 100, count); j++) {
            renames.push(fsp.rename(path.join(inPath, oldName(j)), path.join(inPath, newName(j))));
          }
          await Promise.all(renames);
        }
        await barrier(events, workDir);

        const expected = [];
        for (let i = 0; i < count; i++) {
          expected.push(`${nsfw.actions.RENAMED} ${inPath} ${oldName(i)} ${inPath} ${newName(i)}`);
        }
        // Windows also reports renamed files as modified.
        const actual = events.filter(event => event.directory === inPath && event.action !== nsfw.actions.MODIFIED)
          .map(event => event.action === nsfw.actions.RENAMED ?
            `${event.action} ${event.directory} ${event.oldFile} ${event.newDirectory} ${event.newFile}` :
            `${event.action} ${event.directory} ${event.file}`);
        assert.deepStrictEqual(actual.sort(), expected.sort());
      } finally {
        await watch.stop();
        watch = null;
      }
    });

    it('reports the old and the new directory of a directory renamed in a subdirectory', async function() {
      const inPath = path.join(workDir, 'test4', 'folder4');
      const events = [];
      let watch = await nsfw(workDir, batch => events.push(...batch), { debounceMS: DEBOUNCE });

      try {
        await watch.start();
        await fsp.mkdir(path.join(inPath, 'before'));
        await waitForEvent(events, event => event.action === nsfw.actions.CREATED &&
          event.directory === inPath && event.file === 'before', 'creation of the directory to rename');
        const beforeRename = events.length;
        await fsp.rename(path.join(inPath, 'before'), path.join(inPath, 'after'));
        await barrier(events, workDir);
        // Windows also reports a renamed entry as modified.
        assert.deepStrictEqual(
          events.slice(beforeRename).filter(event => event.action !== nsfw.actions.MODIFIED &&
            [event.file, event.oldFile, event.newFile].some(name => name === 'before' || name === 'after')),
          [{ action: nsfw.actions.RENAMED, kind: nsfw.kinds.DIRECTORY, directory: inPath, oldFile: 'before',
            newDirectory: inPath, newFile: 'after' }]
        );
      } finally {
        await watch.stop();
        watch = null;
      }
    });

    it('reports a rename that only changes the case of the name', async function() {
      const { RENAMED } = nsfw.actions;
      const inPath = path.resolve(workDir, 'test6');
      const oldFile = 'testing6.file';
      const newFile = 'Testing6.file';
      const events = [];
      // From the rename on: FSEvents can still report the fixture written just before the watcher started.
      let offset = 0;
      const about = () => events.slice(offset).filter(event => event.directory === inPath &&
        [event.file, event.oldFile, event.newFile].some(name => name === oldFile || name === newFile));

      let watch = await nsfw(workDir, batch => events.push(...batch), { debounceMS: DEBOUNCE });

      try {
        await watch.start();
        await barrier(events, workDir);
        offset = events.length;
        await fsp.rename(path.join(inPath, oldFile), path.join(inPath, newFile));
        await waitUntil(() => about().some(event => event.action === RENAMED),
          { message: () => `No rename in ${JSON.stringify(events)}` });
        await barrier(events, workDir);
        const expected = { action: RENAMED, directory: inPath, oldFile, newDirectory: inPath, newFile };
        const actual = about().map(event => ({ action: event.action, directory: event.directory,
          oldFile: event.oldFile, newDirectory: event.newDirectory, newFile: event.newFile }));
        // ReadDirectoryChangesW also reports a removal next to the rename.
        if (process.platform === 'win32') {
          assert.ok(actual.some(event => isDeepStrictEqual(event, expected)), JSON.stringify(events));
        } else {
          assert.deepStrictEqual(actual, [expected]);
        }
      } finally {
        await watch.stop();
        watch = null;
      }
    });

    it('can listen for a move event', async function() {
      const file = 'testing.file';
      const srcInPath = path.resolve(workDir, 'test4', 'src');
      const destInPath = path.resolve(workDir, 'test4', 'dest');
      let eventListening = false;
      let deleteEventFound = false;
      let createEventFound = false;
      let renameEventFound = false;
      let extraEventFound = false;

      function findEvent(element) {
        if (!eventListening) {
          return;
        }
        if (
          element.action === nsfw.actions.RENAMED &&
          element.directory === path.resolve(srcInPath) &&
          element.oldFile === file &&
          element.newDirectory === path.resolve(destInPath) &&
          element.newFile === file
        ) {
          renameEventFound = true;
        } else if (
          element.action === nsfw.actions.DELETED &&
          element.directory === path.resolve(srcInPath) &&
          element.file === file
        ) {
          deleteEventFound = true;
        } else if (
          element.action === nsfw.actions.CREATED &&
          element.directory === path.resolve(destInPath) &&
          element.file === file
        ) {
          createEventFound = true;
        } else {
          if (element.file === file) {
            extraEventFound = true;
          }
        }
      }

      const events = [];
      let watch = await nsfw(
        workDir,
        batch => {
          batch.forEach(findEvent);
          events.push(...batch);
        },
        { debounceMS: DEBOUNCE }
      );

      try {
        await watch.start();
        await outputFile(path.join(srcInPath, file), '');
        await mkdirp(path.join(destInPath));
        await barrier(events, workDir);
        eventListening = true;
        await fsp.rename(path.join(srcInPath, file), path.join(destInPath, file));
        await waitUntil(() => renameEventFound || (deleteEventFound && createEventFound),
          { message: () => `No move in ${JSON.stringify(events)}` });
        await barrier(events, workDir);
        eventListening = false;

        // ReadDirectoryChangesW only pairs the two names of a rename within a directory.
        if (process.platform === 'win32') {
          assert.ok(deleteEventFound && createEventFound, JSON.stringify(events));
          assert.ok(!renameEventFound, JSON.stringify(events));
        } else {
          assert.ok(renameEventFound, JSON.stringify(events));
          assert.ok(!deleteEventFound && !createEventFound, JSON.stringify(events));
        }

        assert.ok(!extraEventFound);
      } finally {
        await watch.stop();
        watch = null;
      }
    });

    it('reports a directory moved out as deleted, and moved back with what it holds as created', async function() {
      // Moves to and from a directory outside the watched one, as moving to the Trash and restoring from it does.
      const { CREATED, DELETED } = nsfw.actions;
      const inPath = path.resolve(workDir, 'test7');
      const trash = path.resolve(workDir, 'trash');
      const directory = path.join(inPath, 'trashed');
      await outputFile(path.join(directory, 'inner.file'), 'Inner');
      await outputFile(path.join(directory, 'nested', 'deep.file'), 'Deep');
      await mkdirp(trash);
      const events = [];
      const reported = (action, eventDirectory, file, since = 0) => events.slice(since).some(event =>
        event.action === action && event.directory === eventDirectory && event.file === file);

      let watch = await nsfw(inPath, batch => events.push(...batch), { debounceMS: DEBOUNCE });

      try {
        await watch.start();
        await fsp.rename(directory, path.join(trash, 'trashed'));
        await waitUntil(() => reported(DELETED, inPath, 'trashed'),
          { message: () => `No deletion in ${JSON.stringify(events)}` });
        const movedOut = events.length;
        await fsp.rename(path.join(trash, 'trashed'), directory);
        const created = [[inPath, 'trashed'], [directory, 'inner.file'], [directory, 'nested'],
          [path.join(directory, 'nested'), 'deep.file']];
        await waitUntil(
          () => created.every(([eventDirectory, file]) => reported(CREATED, eventDirectory, file, movedOut)),
          { message: () => `Missing creations in ${JSON.stringify(events)}` });
      } finally {
        await watch.stop();
        watch = null;
      }
    });

    it('can run multiple watchers at once', async function() {
      const dirA = path.resolve(workDir, 'test0');
      const fileA = 'testing1.file';
      const dirB = path.resolve(workDir, 'test1');
      const fileB = 'testing0.file';
      // FSEvents can report the same creation more than once under load, so check that each
      // watcher saw its file instead of counting events.
      let createdA = false;
      let createdB = false;

      function findEvent(element) {
        if (
          element.action === nsfw.actions.CREATED
        ) {
          if (element.directory === dirA && element.file === fileA) {
            createdA = true;
          } else if (element.directory === dirB && element.file === fileB) {
            createdB = true;
          }
        }
      }

      let watchA = await nsfw(
        dirA,
        events => events.forEach(findEvent),
        { debounceMS: DEBOUNCE }
      );
      let watchB = await nsfw(
        dirB,
        events => events.forEach(findEvent),
        { debounceMS: DEBOUNCE }
      );

      try {
        await Promise.all([watchA.start(), watchB.start()]);
        await Promise.all([
          fsp.writeFile(path.join(dirA, fileA), 'At times, sunflower seeds are all that is life.'),
          fsp.writeFile(path.join(dirB, fileB), 'At times, sunflower seeds are all that is life.')
        ]);
        await waitUntil(() => createdA && createdB, { message: () =>
          `watcher ${createdA ? 'B' : 'A'} did not report the created file` });
      } finally {
        await Promise.all([watchA.stop(), watchB.stop()]);
        watchA = null;
        watchB = null;
      }
    });

    it('reports a change made as soon as start() resolves', async function() {
      // No settle time after start(): a watcher that resolves before it is armed loses these.
      // Started concurrently because contention for the watcher threads widens that window.
      const file = 'right_after_start.file';
      const dirs = Array.from({ length: 10 }, (_, i) => path.resolve(workDir, 'test' + i));
      const seen = new Set();
      let watches = await Promise.all(dirs.map(dir => nsfw(
        dir,
        events => events.forEach(element => {
          if (element.action === nsfw.actions.CREATED && element.file === file) {
            seen.add(dir);
          }
        }),
        { debounceMS: DEBOUNCE }
      )));

      try {
        await Promise.all(watches.map((watch, i) => watch.start().then(() =>
          fsp.writeFile(path.join(dirs[i], file), 'Not a moment too soon.')
        )));
        await waitUntil(() => seen.size === dirs.length, { message: () =>
          `Unreported: ${dirs.filter(dir => !seen.has(dir))}` });
      } finally {
        await Promise.all(watches.map(watch => watch.stop()));
        watches = null;
      }
    });

    it('will properly track the movement of watched directories across watched directories', async function() {
      const performRenameProcedure = async (number) => {
        await fsp.mkdir(path.join(workDir, `test${number}`, 'sneaky-folder'));
        await fsp.rename(
          path.join(workDir, `test${number}`, `folder${number}`),
          path.join(workDir, `test${number + 1}`, 'bad-folder')
        );
        await fsp.rename(
          path.join(workDir, `test${number}`, 'sneaky-folder'),
          path.join(workDir, `test${number}`, 'bad-folder')
        );
        await remove(path.join(workDir, `test${number}`));
        await remove(path.join(workDir, `test${number + 1}`));
      };

      const events = [];
      let watch = await nsfw(workDir, batch => events.push(...batch), { debounceMS: DEBOUNCE });

      try {
        await watch.start();
        await Promise.all([
          performRenameProcedure(0),
          performRenameProcedure(2),
          performRenameProcedure(4),
          performRenameProcedure(6),
          performRenameProcedure(8)
        ]);
        await barrier(events, workDir);
      } finally {
        await watch.stop();
        watch = null;
      }
    });
  });

  describe('Single file', function() {
    const inPath = () => path.join(workDir, 'test1');
    const file = 'testing1.file';
    const filePath = () => path.join(inPath(), file);
    const Native = require('node-gyp-build')(path.join(__dirname, '..', '..'));

    // The native addon, telling when it hands an error over: the file watcher may keep it from the caller.
    function errorSpy() {
      const taken = [];
      class Spy extends Native {
        constructor(watchPath, callback, options) {
          super(watchPath, callback, { ...options, errorCallback: error => {
            taken.push(error);
            options.errorCallback(error);
          } });
        }
      }
      return { nativeBinding: Spy, taken };
    }

    async function watchFile(watchPath, options = {}) {
      const events = [];
      const errors = [];
      const watch = await nsfw(watchPath, batch => events.push(...batch),
        { debounceMS: DEBOUNCE, errorCallback: error => errors.push(error), ...options });
      return { watch, events, errors };
    }

    const describeEvents = events => () => JSON.stringify(events);
    const actionsBut = (events, ignored) => events.map(event => event.action).filter(action => action !== ignored);
    // Waits for an event with action about name in directory.
    const reported = (events, action, directory = inPath(), name = file) => waitForEvent(events,
      event => event.action === action && event.directory === directory && event.file === name,
      `action ${action} for ${name}`);

    // Modifies the watched file and waits for it to be reported: events are delivered in order, so whatever happened
    // before has been reported too.
    async function modified(events, watchedPath = filePath(), directory = inPath(), name = file) {
      const offset = events.length;
      await fsp.appendFile(watchedPath, 'barrier');
      await waitUntil(() => events.slice(offset).some(event => event.action === nsfw.actions.MODIFIED &&
        event.directory === directory && event.file === name),
      { message: () => `No modification of ${name} in ${JSON.stringify(events)}` });
    }

    it('reports neither its siblings nor what its directory holds', async function() {
      const { watch, events } = await watchFile(filePath());
      try {
        await watch.start();
        await fsp.writeFile(path.join(inPath(), 'sibling.file'), 'sibling');
        await fsp.writeFile(path.join(inPath(), 'folder1', file), 'same name below');
        await fsp.mkdir(path.join(inPath(), 'folder1', 'nested'));
        await fsp.writeFile(path.join(inPath(), 'folder1', 'nested', file), 'deeper');
        await fsp.rename(path.join(inPath(), 'sibling.file'), path.join(inPath(), 'folder1', 'moved.file'));
        await modified(events);
        assert.ok(events.every(event => event.action === nsfw.actions.MODIFIED && event.directory === inPath() &&
          event.file === file), describeEvents(events)());
      } finally {
        await watch.stop();
      }
    });

    it('holds a single inotify watch, whatever its directory holds', async function(t) {
      if (process.platform !== 'linux') return t.skip();
      for (let i = 0; i < 20; i++) {
        await mkdirp(path.join(inPath(), `sub${i}`, 'nested'));
      }
      const before = countInotifyWatches();
      const { watch, events } = await watchFile(filePath());
      try {
        await watch.start();
        assert.strictEqual(countInotifyWatches(), before + 1);
        await mkdirp(path.join(inPath(), 'created', 'nested'));
        await fsp.rename(path.join(inPath(), 'sub0'), path.join(inPath(), 'renamed'));
        await modified(events);
        assert.strictEqual(countInotifyWatches(), before + 1);
      } finally {
        await watch.stop();
      }
      await watchCountReaches(before);
    });

    it('does not poll the file', async function() {
      const fsPromises = require('fs').promises;
      const { stat, lstat } = fsPromises;
      const calls = [];
      const { watch, events } = await watchFile(filePath(), { debounceMS: 1 });
      fsPromises.stat = (...args) => {
        calls.push(['stat', args[0]]);
        return stat.apply(fsPromises, args);
      };
      fsPromises.lstat = (...args) => {
        calls.push(['lstat', args[0]]);
        return lstat.apply(fsPromises, args);
      };
      try {
        await watch.start();
        calls.length = 0;
        // Nothing looks at the file while it is watched, however long that is.
        await sleep(500);
        await modified(events);
        assert.deepStrictEqual(calls, []);
      } finally {
        fsPromises.stat = stat;
        fsPromises.lstat = lstat;
        await watch.stop();
      }
    });

    it('reports a file renamed over it as modified', async function() {
      const { watch, events } = await watchFile(filePath());
      try {
        await watch.start();
        const temporary = path.join(inPath(), 'testing1.file.tmp');
        await fsp.writeFile(temporary, 'saved atomically');
        await fsp.rename(temporary, filePath());
        await reported(events, nsfw.actions.MODIFIED);
        await modified(events);
        assert.ok(events.every(event => event.action === nsfw.actions.MODIFIED), describeEvents(events)());
      } finally {
        await watch.stop();
      }
    });

    it('reports it renamed away as deleted, and renamed back as created', async function() {
      const { watch, events } = await watchFile(filePath());
      try {
        await watch.start();
        const away = path.join(inPath(), 'away.file');
        await fsp.rename(filePath(), away);
        await reported(events, nsfw.actions.DELETED);
        await fsp.rename(away, filePath());
        await reported(events, nsfw.actions.CREATED);
        await modified(events);
        assert.deepStrictEqual(actionsBut(events, nsfw.actions.MODIFIED), [nsfw.actions.DELETED, nsfw.actions.CREATED]);
      } finally {
        await watch.stop();
      }
    });

    it('uses a removal received while the startup stat settles to classify a later creation', async function() {
      let removed = false;
      class Spy extends Native {
        constructor(watchPath, callback, options) {
          super(watchPath, batch => {
            callback(batch);
            removed ||= batch.some(event => event.file === file && event.action === nsfw.actions.DELETED);
          }, options);
        }
      }
      const { watch, events } = await watchFile(filePath(), { nativeBinding: Spy });
      const { stat } = fsp;
      let observed = false;
      let release;
      const held = new Promise(resolve => { release = resolve; });
      fsp.stat = async (...args) => {
        const snapshot = await stat(...args);
        if (path.basename(args[0]) === file) {
          observed = true;
          await held;
        }
        return snapshot;
      };
      try {
        const started = watch.start();
        await waitUntil(() => observed, { message: 'start() did not stat the watched file' });
        await fsp.unlink(filePath());
        await waitUntil(() => removed, { message: 'Native watcher did not report the removal' });
        release();
        await started;
        assert.deepStrictEqual(events, []);
        await fsp.writeFile(filePath(), 'back');
        await waitUntil(() => events.length > 0, { message: 'No event after the file was recreated' });
        assert.strictEqual(events[0].action, nsfw.actions.CREATED);
      } finally {
        fsp.stat = stat;
        release();
        await watch.stop();
      }
    });

    it('takes a name with the characters of patterns literally', async function() {
      const name = process.platform === 'win32' ? '[ab] !#x.file' : '[ab] !#*?\\x.file';
      const watchedPath = path.join(inPath(), name);
      await fsp.writeFile(watchedPath, 'special');
      // What the name matches as a pattern, which is not the file.
      const lookalike = process.platform === 'win32' ? 'a !#x.file' : 'a !#yzx.file';
      const { watch, events } = await watchFile(watchedPath);
      try {
        await watch.start();
        await fsp.writeFile(path.join(inPath(), lookalike), 'lookalike');
        await modified(events, watchedPath, inPath(), name);
        assert.ok(events.every(event => event.file === name), describeEvents(events)());
      } finally {
        await watch.stop();
      }
    });

    it('watches the file a symlink leads to, under the symlink\'s name', async function(t) {
      if (process.platform === 'win32') return t.skip();
      const target = path.join(workDir, 'test2', 'testing2.file');
      const link = path.join(inPath(), 'link.file');
      await fsp.symlink(target, link, 'file');
      const { watch, events } = await watchFile(link);
      try {
        await watch.start();
        await untilNextSecond(target);
        await fsp.writeFile(target, 'through the target');
        await reported(events, nsfw.actions.MODIFIED, inPath(), 'link.file');
        await fsp.writeFile(path.join(workDir, 'test2', 'sibling.file'), 'sibling');
        await modified(events, link, inPath(), 'link.file');
        assert.ok(events.every(event => event.directory === inPath() && event.file === 'link.file'),
          describeEvents(events)());
      } finally {
        await watch.stop();
      }
    });

    it('reports nothing under the symlinks next to it, there before it started or made since', async function(t) {
      if (process.platform === 'win32') return t.skip();
      await fsp.symlink(file, path.join(inPath(), 'before.link'), 'file');
      await fsp.symlink('missing.file', path.join(inPath(), 'dangling.link'), 'file');
      const { watch, events } = await watchFile(filePath());
      try {
        await watch.start();
        await fsp.symlink(file, path.join(inPath(), 'after.link'), 'file');
        await modified(events);
        // Written through a symlink, it is the file that changes.
        await modified(events, path.join(inPath(), 'before.link'));
        await modified(events, path.join(inPath(), 'after.link'));
        await fsp.writeFile(path.join(inPath(), 'missing.file'), 'no longer dangling');
        await modified(events);
        assert.ok(events.every(event => event.action === nsfw.actions.MODIFIED && event.directory === inPath() &&
          event.file === file), describeEvents(events)());
      } finally {
        await watch.stop();
      }
    });

    it('reports a symlink renamed over it as modified, and a file renamed over that symlink', async function(t) {
      if (process.platform === 'win32') return t.skip();
      const { watch, events } = await watchFile(filePath());
      try {
        await watch.start();
        const temporary = path.join(inPath(), 'testing1.file.tmp');
        await fsp.writeFile(path.join(inPath(), 'led-to.file'), 'led to');
        await fsp.symlink('led-to.file', temporary, 'file');
        await fsp.rename(temporary, filePath());
        await reported(events, nsfw.actions.MODIFIED);
        await fsp.writeFile(temporary, 'a file again');
        const offset = events.length;
        await fsp.rename(temporary, filePath());
        await waitUntil(() => events.slice(offset).some(event => event.action === nsfw.actions.MODIFIED),
          { message: () => `The file renamed over the symlink was not reported: ${JSON.stringify(events)}` });
        await modified(events);
        assert.ok(events.every(event => event.action === nsfw.actions.MODIFIED && event.directory === inPath() &&
          event.file === file), describeEvents(events)());
      } finally {
        await watch.stop();
      }
    });

    // The native addon, whose watcher can be made to fail as on an overflow of its event queue, and whose start() can
    // be made to fail as for a directory that can't be watched.
    function failingBinding() {
      // held: a promise start() waits for before starting.
      const control = { instances: [], starts: 0, startFailures: 0, held: undefined };
      class Failing extends Native {
        constructor(watchPath, callback, options) {
          super(watchPath, callback, options);
          this.reportError = options.errorCallback;
          control.instances.push(this);
        }
        async start() {
          control.starts++;
          await control.held;
          if (control.startFailures > 0) {
            control.startFailures--;
            throw new Error('NSFW was unable to start watching that directory.');
          }
          return super.start();
        }
        async fail(error) {
          await super.stop();
          this.reportError(error);
        }
      }
      control.fail = (message = 'Buffer filled up and service needs a restart') =>
        control.instances[0].fail(new Error(message));
      return { nativeBinding: Failing, control };
    }

    it('reports its directory renamed away as deleted, and renamed back as created', async function(t) {
      // Windows doesn't let a directory go while it is watched.
      if (process.platform === 'win32') return t.skip();
      const { watch, events, errors } = await watchFile(filePath());
      try {
        await watch.start();
        const away = path.join(workDir, 'away');
        await fsp.rename(inPath(), away);
        await reported(events, nsfw.actions.DELETED);
        // Looked for at least once while gone.
        await sleep(1500);
        await fsp.rename(away, inPath());
        await reported(events, nsfw.actions.CREATED);
        await modified(events);
        assert.deepStrictEqual(actionsBut(events, nsfw.actions.MODIFIED), [nsfw.actions.DELETED, nsfw.actions.CREATED]);
        assert.deepStrictEqual(errors, []);
      } finally {
        await watch.stop();
      }
    });

    it('reports its directory removed as deleted, and created again with the file as created', async function(t) {
      // Windows doesn't let a directory go while it is watched.
      if (process.platform === 'win32') return t.skip();
      const { watch, events, errors } = await watchFile(filePath());
      try {
        await watch.start();
        await remove(inPath());
        await reported(events, nsfw.actions.DELETED);
        await sleep(1500);
        await mkdirp(inPath());
        await fsp.writeFile(filePath(), 'back');
        await reported(events, nsfw.actions.CREATED);
        await modified(events);
        assert.deepStrictEqual(actionsBut(events, nsfw.actions.MODIFIED), [nsfw.actions.DELETED, nsfw.actions.CREATED]);
        assert.deepStrictEqual(errors, []);
      } finally {
        await watch.stop();
      }
    });

    it('throws nothing without an errorCallback when its directory goes', async function(t) {
      // Windows doesn't let a directory go while it is watched.
      if (process.platform === 'win32') return t.skip();
      const events = [];
      const watch = await nsfw(filePath(), batch => events.push(...batch), { debounceMS: DEBOUNCE });
      const uncaught = [];
      const onUncaught = error => uncaught.push(error);
      process.on('uncaughtException', onUncaught);
      try {
        await watch.start();
        await remove(inPath());
        await reported(events, nsfw.actions.DELETED);
        await sleep(1500);
        await mkdirp(inPath());
        await fsp.writeFile(filePath(), 'back');
        await reported(events, nsfw.actions.CREATED);
        assert.deepStrictEqual(uncaught, []);
      } finally {
        process.off('uncaughtException', onUncaught);
        await watch.stop();
      }
    });

    it('holds no inotify watch while its directory is gone', async function(t) {
      if (process.platform !== 'linux') return t.skip();
      const before = countInotifyWatches();
      const { watch, events } = await watchFile(filePath());
      try {
        await watch.start();
        assert.strictEqual(countInotifyWatches(), before + 1);
        await remove(inPath());
        await reported(events, nsfw.actions.DELETED);
        await watchCountReaches(before);
        // Through a few looks for the directory, none of which takes a watch.
        const deadline = Date.now() + 2500;
        while (Date.now() < deadline) {
          assert.strictEqual(countInotifyWatches(), before);
          await sleep(20);
        }
        await mkdirp(inPath());
        await fsp.writeFile(filePath(), 'back');
        await reported(events, nsfw.actions.CREATED);
        assert.strictEqual(countInotifyWatches(), before + 1);
      } finally {
        await watch.stop();
      }
      await watchCountReaches(before);
    });

    it('pauses, resumes, stops and starts while its directory is gone', async function(t) {
      // Windows doesn't let a directory go while it is watched.
      if (process.platform === 'win32') return t.skip();
      const { watch, events, errors } = await watchFile(filePath());
      try {
        await watch.start();
        await remove(inPath());
        await reported(events, nsfw.actions.DELETED);

        // Nothing is reported while paused, not even the directory coming back.
        await watch.pause();
        let offset = events.length;
        await mkdirp(inPath());
        await fsp.writeFile(filePath(), 'back while paused');
        await sleep(1500);
        assert.deepStrictEqual(events.slice(offset), []);
        await watch.resume();
        await modified(events);

        // Nothing is reported once stopped either.
        await remove(inPath());
        await reported(events, nsfw.actions.DELETED);
        await watch.stop();
        offset = events.length;
        await mkdirp(inPath());
        await fsp.writeFile(filePath(), 'back while stopped');
        await sleep(1500);
        assert.deepStrictEqual(events.slice(offset), []);

        // start() takes a missing directory.
        await remove(inPath());
        await watch.start();
        await mkdirp(inPath());
        await fsp.writeFile(filePath(), 'back once started');
        await waitUntil(() => events.slice(offset).some(event => event.action === nsfw.actions.CREATED),
          { message: describeEvents(events) });
        await modified(events);
        assert.deepStrictEqual(errors, []);
      } finally {
        await watch.stop();
      }
    });

    it('recovers from a failure of the watcher of its directory, reporting what changed meanwhile', async function() {
      const { nativeBinding, control } = failingBinding();
      const { watch, events, errors } = await watchFile(filePath(), { nativeBinding });
      try {
        await watch.start();
        await control.fail();
        await fsp.appendFile(filePath(), 'unseen by the failed watcher');
        await reported(events, nsfw.actions.MODIFIED);
        await modified(events);

        await control.fail();
        await fsp.unlink(filePath());
        await reported(events, nsfw.actions.DELETED);
        await waitUntil(() => control.starts === 3, { message: 'Not watched again' });
        await fsp.writeFile(filePath(), 'back');
        await reported(events, nsfw.actions.CREATED);
        await modified(events);
        assert.deepStrictEqual(actionsBut(events, nsfw.actions.MODIFIED), [nsfw.actions.DELETED, nsfw.actions.CREATED]);
        assert.deepStrictEqual(errors, []);
      } finally {
        await watch.stop();
      }
    });

    it('reports nothing after a failure of the watcher of its directory if the file did not change', async function() {
      const { nativeBinding, control } = failingBinding();
      const { watch, events, errors } = await watchFile(filePath(), { nativeBinding });
      try {
        await watch.start();
        await control.fail();
        await waitUntil(() => control.starts === 2, { message: 'Not watched again' });
        await sleep(DEBOUNCE * 4);
        assert.deepStrictEqual([events, errors], [[], []]);
        await modified(events);
      } finally {
        await watch.stop();
      }
    });

    it('tells errorCallback once that its directory can\'t be watched again, and keeps trying', async function() {
      const { nativeBinding, control } = failingBinding();
      const { watch, events, errors } = await watchFile(filePath(), { nativeBinding });
      try {
        await watch.start();
        control.startFailures = 2;
        await control.fail();
        await waitUntil(() => control.starts === 4, { message: 'Not watched again' });
        await modified(events);
        assert.deepStrictEqual(errors.map(error => error.message),
          ['NSFW was unable to start watching that directory.']);
      } finally {
        await watch.stop();
      }
    });

    it('tells errorCallback again of a later failure to watch its directory after watching it again', async function() {
      const { nativeBinding, control } = failingBinding();
      const { watch, events, errors } = await watchFile(filePath(), { nativeBinding });
      try {
        await watch.start();
        control.startFailures = 1;
        await control.fail();
        await waitUntil(() => control.starts === 3, { message: 'Not watched again' });
        await modified(events);
        control.startFailures = 1;
        await control.fail();
        await waitUntil(() => errors.length === 2, { message: 'The second failure was not told' });
      } finally {
        await watch.stop();
      }
    });

    it('tells errorCallback again of a later failure to watch its directory once it was restarted',
      async function() {
        const { nativeBinding, control } = failingBinding();
        const { watch, events, errors } = await watchFile(filePath(), { nativeBinding });
        try {
          await watch.start();
          control.startFailures = 1;
          await control.fail();
          await waitUntil(() => errors.length === 1, { message: 'The first failure was not told' });
          await watch.stop();
          await watch.start();
          await modified(events);
          control.startFailures = 1;
          await control.fail();
          await waitUntil(() => errors.length === 2, { message: 'The second failure was not told' });
        } finally {
          await watch.stop();
        }
      });

    it('reports nothing once stop() was called while its directory is being watched again', async function() {
      const { nativeBinding, control } = failingBinding();
      const { watch, events, errors } = await watchFile(filePath(), { nativeBinding });
      let release;
      try {
        await watch.start();
        control.held = new Promise(resolve => { release = resolve; });
        await control.fail();
        await fsp.appendFile(filePath(), 'unseen by the failed watcher');
        await waitUntil(() => control.starts === 2, { message: 'Not watched again' });
        const stopped = watch.stop();
        release();
        await stopped;
        await sleep(DEBOUNCE * 4);
        assert.deepStrictEqual([events, errors], [[], []]);
      } finally {
        release?.();
        await watch.stop();
      }
    });

    for (const [what, stalledName] of [['its file', () => file], ['its directory', () => path.basename(inPath())]]) {
      it(`stops and pauses while a look at ${what} after a failure is stalled`, async function() {
        const { nativeBinding, control } = failingBinding();
        const { watch, events } = await watchFile(filePath(), { nativeBinding });
        const fsPromises = require('fs').promises;
        const { stat } = fsPromises;
        let stalled = false;
        let release;
        const held = new Promise(resolve => { release = resolve; });
        try {
          await watch.start();
          // As on a share that stopped answering.
          fsPromises.stat = async (...args) => {
            if (path.basename(String(args[0])) === stalledName()) {
              stalled = true;
              await held;
            }
            return stat.apply(fsPromises, args);
          };
          await control.fail();
          await waitUntil(() => stalled, { message: `${what} was not looked at` });
          await watch.pause();
          await watch.stop();
          // What the stalled look finds once it returns is not reported.
          await fsp.unlink(filePath());
          fsPromises.stat = stat;
          release();
          await sleep(DEBOUNCE * 4);
          assert.deepStrictEqual(events, []);
          await fsp.writeFile(filePath(), 'back');
          await watch.start();
          await modified(events);
        } finally {
          fsPromises.stat = stat;
          release();
          await watch.stop();
        }
      });
    }

    it('reports on resume the removal a pause kept the look after a failure from reporting', async function() {
      const { nativeBinding, control } = failingBinding();
      const { watch, events, errors } = await watchFile(filePath(), { nativeBinding });
      const fsPromises = require('fs').promises;
      const { stat } = fsPromises;
      let stalled = false;
      let release;
      const held = new Promise(resolve => { release = resolve; });
      try {
        await watch.start();
        fsPromises.stat = async (...args) => {
          if (path.basename(String(args[0])) === file) {
            stalled = true;
            await held;
          }
          return stat.apply(fsPromises, args);
        };
        await control.fail();
        await waitUntil(() => stalled, { message: 'The file was not looked at' });
        await watch.pause();
        await fsp.unlink(filePath());
        fsPromises.stat = stat;
        release();
        await sleep(DEBOUNCE * 4);
        assert.deepStrictEqual(events, []);
        await watch.resume();
        await reported(events, nsfw.actions.DELETED);
        await fsp.writeFile(filePath(), 'back');
        await reported(events, nsfw.actions.CREATED);
        await modified(events);
        assert.deepStrictEqual(actionsBut(events, nsfw.actions.MODIFIED), [nsfw.actions.DELETED, nsfw.actions.CREATED]);
        assert.deepStrictEqual(errors, []);
      } finally {
        fsPromises.stat = stat;
        release();
        await watch.stop();
      }
    });

    it('takes a file changed while paused as it is on resume, after a pause cut the look after a failure short',
      async function() {
        const { nativeBinding, control } = failingBinding();
        const { watch, events, errors } = await watchFile(filePath(), { nativeBinding });
        const witnessEvents = [];
        const witness = await nsfw(inPath(), batch => witnessEvents.push(...batch), { debounceMS: DEBOUNCE });
        const fsPromises = require('fs').promises;
        const { stat } = fsPromises;
        let stalled = false;
        let release;
        const held = new Promise(resolve => { release = resolve; });
        try {
          await watch.start();
          fsPromises.stat = async (...args) => {
            if (path.basename(String(args[0])) === file) {
              stalled = true;
              await held;
            }
            return stat.apply(fsPromises, args);
          };
          await control.fail();
          await waitUntil(() => stalled, { message: 'The file was not looked at' });
          await watch.pause();
          await witness.start();
          await fsp.appendFile(filePath(), 'changed while paused');
          // Once a watcher started before it reports the change, FSEvents won't report it to one started after.
          await waitForEvent(witnessEvents, event => event.file === file, 'change seen by the witness');
          fsPromises.stat = stat;
          release();
          await watch.resume();
          await sleep(DEBOUNCE * 4);
          assert.deepStrictEqual(events, []);
          await modified(events);
          assert.deepStrictEqual(errors, []);
        } finally {
          fsPromises.stat = stat;
          release();
          await watch.stop();
          await witness.stop().catch(() => {});
        }
      });

    it('reports nothing of a failure while paused, and watches again once resumed', async function() {
      const { nativeBinding, control } = failingBinding();
      const { watch, events, errors } = await watchFile(filePath(), { nativeBinding });
      try {
        await watch.start();
        await watch.pause();
        await control.fail();
        await fsp.unlink(filePath());
        await sleep(1500);
        assert.deepStrictEqual([events, errors, control.starts], [[], [], 1]);
        await fsp.writeFile(filePath(), 'back');
        await watch.resume();
        await modified(events);
        assert.deepStrictEqual(errors, []);
      } finally {
        await watch.stop();
      }
    });

    it('watches again after being stopped while paused', async function() {
      const { watch, events } = await watchFile(filePath());
      try {
        await watch.start();
        await watch.pause();
        await watch.stop();
        await watch.start();
        await modified(events);
      } finally {
        await watch.stop();
      }
    });

    it('rejects start() when its directory is there but can\'t be watched', async function() {
      const { nativeBinding, control } = failingBinding();
      const { watch, events } = await watchFile(filePath(), { nativeBinding });
      control.startFailures = 1;
      await assert.rejects(watch.start(), /unable to start watching/);
      try {
        await watch.start();
        await modified(events);
      } finally {
        await watch.stop();
      }
    });

    it('delivers nothing once stopped, even as its directory goes', async function(t) {
      // Windows doesn't let a directory go while it is watched.
      if (process.platform === 'win32') return t.skip();
      const fsPromises = require('fs').promises;
      const { stat } = fsPromises;
      const { watch, events, errors } = await watchFile(filePath());
      // Holds the look the error takes at the file, so that stop() comes between the error and its delivery.
      let looked = false;
      let release;
      const held = new Promise(resolve => { release = resolve; });
      try {
        await watch.start();
        fsPromises.stat = async (...args) => {
          if (args[0] === filePath()) {
            looked = true;
            await held;
          }
          return stat.apply(fsPromises, args);
        };
        await remove(inPath());
        await waitUntil(() => looked, { message: 'The error was not delivered' });
        // What the native watcher reported before the error, the file's own removal among them, may be delivered.
        const delivered = events.length;
        const stopped = watch.stop();
        release();
        await stopped;
        await sleep(DEBOUNCE * 4);
        assert.deepStrictEqual([events.slice(delivered), errors], [[], []]);
      } finally {
        fsPromises.stat = stat;
        release();
        await watch.stop();
      }
    });

    for (const [when, paused] of [['resuming', true], ['starting', false]]) {
      it(`reports nothing once stop() was called before its directory goes, on a ${when} watcher`, async function(t) {
        // Windows doesn't let a directory go while it is watched.
        if (process.platform === 'win32') return t.skip();
        const fsPromises = require('fs').promises;
        const { stat } = fsPromises;
        const spy = errorSpy();
        const { watch, events, errors } = await watchFile(filePath(), { nativeBinding: spy.nativeBinding });
        let looked = false;
        let release;
        const held = new Promise(resolve => { release = resolve; });
        try {
          if (paused) {
            await watch.start();
            await watch.pause();
          }
          // Holds the look start() takes at the file, so that stop() waits behind it while the directory goes.
          fsPromises.stat = async (...args) => {
            if (args[0] === filePath()) {
              looked = true;
              await held;
            }
            return stat.apply(fsPromises, args);
          };
          const started = watch.start();
          const stopping = watch.stop();
          await waitUntil(() => looked, { message: 'start() did not look at the file' });
          const delivered = events.length;
          await remove(inPath());
          await waitUntil(() => spy.taken.length > 0, { message: 'The removal of the directory was not reported' });
          release();
          await Promise.all([started, stopping]);
          await sleep(DEBOUNCE * 4);
          assert.deepStrictEqual([events.slice(delivered), errors], [[], []]);
        } finally {
          fsPromises.stat = stat;
          release();
          await watch.stop();
        }
      });
    }

    it('keeps watching through a resume() called before its directory goes', async function(t) {
      // Windows doesn't let a directory go while it is watched.
      if (process.platform === 'win32') return t.skip();
      const fsPromises = require('fs').promises;
      const { stat } = fsPromises;
      const spy = errorSpy();
      const { watch, events, errors } = await watchFile(filePath(), { nativeBinding: spy.nativeBinding });
      let looked = false;
      let release;
      const held = new Promise(resolve => { release = resolve; });
      try {
        // Holds the look start() takes at the file, so that resume() waits behind it while the directory goes.
        fsPromises.stat = async (...args) => {
          if (args[0] === filePath()) {
            looked = true;
            await held;
          }
          return stat.apply(fsPromises, args);
        };
        const started = watch.start();
        await waitUntil(() => looked, { message: 'start() did not look at the file' });
        const resumed = watch.resume().then(() => null, error => error);
        await remove(inPath());
        await waitUntil(() => spy.taken.length > 0, { message: 'The removal of the directory was not reported' });
        release();
        await started;
        await resumed;
        fsPromises.stat = stat;
        await mkdirp(inPath());
        await fsp.writeFile(filePath(), 'back');
        await reported(events, nsfw.actions.CREATED);
        assert.strictEqual(await resumed, null);
        assert.deepStrictEqual(errors, []);
      } finally {
        fsPromises.stat = stat;
        release();
        await watch.stop();
      }
    });

    it('starts, stops, pauses and resumes whatever state it is in', async function() {
      const { watch, events } = await watchFile(filePath());
      await watch.stop();
      await watch.pause();
      await watch.resume();
      try {
        await watch.start();
        await modified(events);
        await watch.pause();
        await watch.pause();
        const offset = events.length;
        await fsp.appendFile(filePath(), 'unseen');
        await sleep(DEBOUNCE * 4);
        assert.deepStrictEqual(events.slice(offset), []);
        await watch.stop();
        await watch.start();
        await modified(events);
        await Promise.all([watch.stop(), watch.start()]);
        await modified(events);
      } finally {
        await watch.stop();
        await watch.stop();
      }
    });

    it('takes a debounce longer than a directory watcher does', async function() {
      const watch = await nsfw(filePath(), () => {}, { debounceMS: 120000 });
      await watch.start();
      await watch.stop();
    });
  });

  describe('Kinds', function() {
    const { FILE, DIRECTORY, UNKNOWN } = nsfw.kinds;
    const { CREATED, DELETED, MODIFIED } = nsfw.actions;

    it('exports the kinds', function() {
      assert.deepStrictEqual(nsfw.kinds, { FILE: 0, DIRECTORY: 1, UNKNOWN: 2 });
    });

    // The names an event is about, relative to directory.
    function namesOf(event, directory) {
      return event.action === nsfw.actions.RENAMED ?
        [path.relative(directory, path.join(event.directory, event.oldFile)),
          path.relative(directory, path.join(event.newDirectory, event.newFile))] :
        [path.relative(directory, path.join(event.directory, event.file))];
    }

    // Runs each step in a new directory of the watched one, then a barrier, and checks that every event about a name
    // in kinds tells the kind kinds has for it, or UNKNOWN where unknownAllowed(event, step) says the system can't
    // tell. expected: [step, name, actions] that each step must have reported at least once. setup(inPath) runs before
    // the watcher starts.
    async function checkKinds(kinds, steps,
      { expected = [], unknownAllowed = () => false, options = {}, setup = async () => {} } = {}) {
      const inPath = path.join(workDir, 'kinds');
      await fsp.mkdir(inPath);
      await setup(inPath);
      const events = [];
      let watch = await nsfw(workDir, batch => events.push(...batch), { debounceMS: DEBOUNCE, ...options });
      const stepEvents = [];
      try {
        await watch.start();
        for (const step of steps) {
          const first = events.length;
          await step(inPath);
          await barrier(events, workDir);
          stepEvents.push(events.slice(first));
        }
      } finally {
        await watch.stop();
        watch = null;
      }
      stepEvents.forEach((batch, step) => {
        for (const event of batch) {
          const names = namesOf(event, inPath).filter(name => name in kinds);
          if (names.length === 0) {
            continue;
          }
          const kind = kinds[names[names.length - 1]];
          assert.ok(event.kind === kind || (event.kind === UNKNOWN && unknownAllowed(event, step)),
            `Expected kind ${kind} in step ${step} for ${JSON.stringify(event)} in ${JSON.stringify(events)}`);
        }
      });
      for (const [step, name, actions] of expected) {
        assert.ok(stepEvents[step].some(event => actions.includes(event.action) &&
          namesOf(event, inPath).at(-1) === name), `No ${actions} of ${name} in ${JSON.stringify(stepEvents[step])}`);
      }
      return stepEvents;
    }

    it('tells files from directories when they are created, modified, renamed and deleted', async function() {
      const past = new Date(Date.now() - 3600 * 1000);
      const stepEvents = await checkKinds({ 'file': FILE, 'file2': FILE, 'dir': DIRECTORY, 'dir2': DIRECTORY }, [
        async inPath => {
          await fsp.writeFile(path.join(inPath, 'file'), 'created');
          await fsp.mkdir(path.join(inPath, 'dir'));
        },
        async inPath => {
          await untilNextSecond(path.join(inPath, 'file'));
          await fsp.writeFile(path.join(inPath, 'file'), 'modified');
          await fsp.utimes(path.join(inPath, 'dir'), past, past);
        },
        async inPath => {
          await fsp.rename(path.join(inPath, 'file'), path.join(inPath, 'file2'));
          await fsp.rename(path.join(inPath, 'dir'), path.join(inPath, 'dir2'));
        },
        async inPath => {
          await fsp.rm(path.join(inPath, 'file2'));
          await fsp.rmdir(path.join(inPath, 'dir2'));
        },
        () => fsp.utimes(workDir, past, past)
      ], {
        expected: [[0, 'file', [CREATED]], [0, 'dir', [CREATED]], [3, 'file2', [DELETED]],
          [3, 'dir2', [DELETED]]],
        // Windows doesn't tell what a removed entry was, nor what one that is gone when its event is read is. A rename
        // whose new name comes too late after its old one is a removal and a creation.
        unknownAllowed: (event, step) => process.platform === 'win32' && (step === 3 || event.action === DELETED)
      });
      if (process.platform === 'win32') {
        assert.ok(stepEvents[3].some(event => event.action === DELETED && event.kind === UNKNOWN));
      }
      // Linux reports a change of the watched directory itself, with an empty name.
      if (process.platform === 'linux') {
        assert.ok(stepEvents[4].some(event => event.action === MODIFIED && event.directory === workDir &&
          event.file === '' && event.kind === DIRECTORY), JSON.stringify(stepEvents[4]));
      }
    });

    it('tells what a directory moved in holds', async function() {
      const outside = path.join(workDir, 'test0');
      await fsp.mkdir(path.join(outside, 'moved', 'sub'), { recursive: true });
      await fsp.writeFile(path.join(outside, 'moved', 'sub', 'inner.file'), 'inner');
      const kinds = {
        'moved': DIRECTORY,
        [path.join('moved', 'sub')]: DIRECTORY,
        [path.join('moved', 'sub', 'inner.file')]: FILE
      };
      // What a directory that appeared holds is reported too.
      const expected = [[0, 'moved', [CREATED]], [0, path.join('moved', 'sub', 'inner.file'), [CREATED]]];
      // From a directory that isn't watched, so that it is a move in.
      await checkKinds(kinds, [inPath => fsp.rename(path.join(outside, 'moved'), path.join(inPath, 'moved'))],
        { expected, options: { excludedPatterns: ['/test0'] } });
    });

    it('keeps the kind of a rename the filter turns into a removal or a creation', async function() {
      await checkKinds({ 'file': FILE, 'dir': DIRECTORY, 'back.file': FILE, 'back.dir': DIRECTORY }, [
        async inPath => {
          await fsp.writeFile(path.join(inPath, 'file'), 'created');
          await fsp.mkdir(path.join(inPath, 'dir'));
        },
        async inPath => {
          await fsp.rename(path.join(inPath, 'file'), path.join(inPath, 'ignored.file'));
          await fsp.rename(path.join(inPath, 'dir'), path.join(inPath, 'ignored.dir'));
        },
        async inPath => {
          await fsp.rename(path.join(inPath, 'ignored.file'), path.join(inPath, 'back.file'));
          await fsp.rename(path.join(inPath, 'ignored.dir'), path.join(inPath, 'back.dir'));
        }
      ], {
        expected: [[1, 'file', [DELETED]], [1, 'dir', [DELETED]], [2, 'back.file', [CREATED]],
          [2, 'back.dir', [CREATED]]],
        options: { excludedPatterns: ['ignored.*'] },
        unknownAllowed: event => process.platform === 'win32' && event.action === DELETED
      });
    });

    if (process.platform === 'win32') {
      it('reports a junction to a directory as a directory', async function() {
        await checkKinds({ 'junction': DIRECTORY }, [
          inPath => fsp.symlink(path.join(workDir, 'test1'), path.join(inPath, 'junction'), 'junction'),
          inPath => fsp.rmdir(path.join(inPath, 'junction'))
        ], {
          expected: [[0, 'junction', [CREATED]], [1, 'junction', [DELETED]]],
          unknownAllowed: (event, step) => step === 1
        });
      });
    } else {
      it('reports a symlink as a file, whatever it leads to', async function() {
        const target = await fsp.mkdtemp(path.join(os.tmpdir(), 'nsfw-kinds-'));
        const past = new Date(Date.now() - 3600 * 1000);
        try {
          const expected = [[0, 'link-to-file', [CREATED]], [2, 'link-to-dir', [DELETED]],
            [2, 'dangling', [DELETED]]];
          // The tree follows a symlink to a directory that was there when it started: what happens to that directory
          // is reported at the symlink.
          if (process.platform === 'linux') {
            expected.push([1, 'link-to-dir', [MODIFIED]]);
          }
          await checkKinds({ 'link-to-dir': FILE, 'link-to-file': FILE, 'dangling': FILE }, [
            async inPath => {
              await fsp.symlink(path.join(workDir, 'test1', 'testing1.file'), path.join(inPath, 'link-to-file'));
              await fsp.symlink(path.join(workDir, 'nowhere'), path.join(inPath, 'dangling'));
            },
            () => fsp.utimes(target, past, past),
            async inPath => {
              for (const name of ['link-to-dir', 'link-to-file', 'dangling']) {
                await fsp.unlink(path.join(inPath, name));
              }
            }
          ], { expected, setup: inPath => fsp.symlink(target, path.join(inPath, 'link-to-dir')) });
        } finally {
          await remove(target);
        }
      });
    }

    it('reports a watched file as a file', async function() {
      const filePath = path.join(workDir, 'test1', 'testing1.file');
      const events = [];
      let watch = await nsfw(filePath, batch => events.push(...batch), { debounceMS: DEBOUNCE });
      try {
        await watch.start();
        await untilNextSecond(filePath);
        await fsp.writeFile(filePath, 'modified');
        await remove(filePath);
        await waitUntil(() => events.some(event => event.action === DELETED),
          { message: () => `No removal in ${JSON.stringify(events)}` });
      } finally {
        await watch.stop();
        watch = null;
      }
      assert.ok(events.every(event => event.kind === FILE), JSON.stringify(events));
    });
  });

  describe('Recursive', function() {
    it('can listen for the destruction of a directory and its subtree', async function() {
      const inPath = path.resolve(workDir, 'test4');
      let deletionCount = 0;

      function findEvent(element) {
        if (element.action === nsfw.actions.DELETED)
        {
          if (element.directory === path.resolve(inPath)
              && (element.file === 'testing4.file' || element.file === 'folder4'))
          {
            deletionCount++;
          }
          else if (element.directory === workDir && element.file === 'test4')
          {
            deletionCount++;
          }
        }
      }

      let watch = await nsfw(
        workDir,
        events => events.forEach(findEvent),
        { debounceMS: DEBOUNCE }
      );

      try {
        await watch.start();
        await remove(inPath);
        await waitUntil(() => deletionCount > 2, { message: () => `${deletionCount} deletions` });
      } finally {
        await watch.stop();
        watch = null;
      }
    });

    if (process.platform === 'linux') {
      // The scan of a new directory races with what is created in it right after it is watched.
      it('reports a file created right after its new directory once', async function() {
        const runs = 50;
        const events = [];
        let watch = await nsfw(
          workDir,
          batch => events.push(...batch),
          { debounceMS: DEBOUNCE }
        );

        try {
          await watch.start();
          for (let i = 0; i < runs; ++i) {
            const created = path.join(workDir, `created${i}`);
            fs.mkdirSync(created);
            fs.writeFileSync(path.join(created, 'f.txt'), 'x');
            const recreated = path.join(workDir, `recreated${i}`);
            fs.mkdirSync(recreated);
            fs.writeFileSync(path.join(recreated, 'f.txt'), 'x');
            fs.rmSync(path.join(recreated, 'f.txt'), { recursive: true, force: true });
            fs.writeFileSync(path.join(recreated, 'f.txt'), 'y');
          }

          const { CREATED, DELETED, MODIFIED } = nsfw.actions;
          const actions = directory => events.filter(event =>
            event.directory === path.join(workDir, directory) && event.file === 'f.txt' && event.action !== MODIFIED
          ).map(event => event.action);
          // Each directory's scan may report its file after the barrier, so wait for every final creation first.
          await waitUntil(() => Array.from({ length: runs }, (_, i) => [`created${i}`, `recreated${i}`])
            .flat().every(directory => actions(directory).at(-1) === CREATED),
          { message: () => `Unreported creations in ${JSON.stringify(events)}` });
          await barrier(events, workDir);
          for (let i = 0; i < runs; ++i) {
            assert.deepEqual(actions(`created${i}`), [CREATED], `created${i}`);
            // What happened before the directory was watched and is gone by its scan goes unseen.
            const recreated = actions(`recreated${i}`);
            assert.ok([[CREATED, DELETED, CREATED], [DELETED, CREATED], [CREATED]].some(expected =>
              isDeepStrictEqual(expected, recreated)
            ), `recreated${i}: ${recreated}`);
          }
        } finally {
          await watch.stop();
          watch = null;
        }
      });

    }

    for (const how of ['moved', 'copied']) {
      it(`reports once each entry of a directory ${how} into the watched one, but not the ignored ones`,
        async function() {
          const inPath = path.join(workDir, 'test4');
          const source = path.join(workDir, 'test5', 'arriving');
          const target = path.join(inPath, 'arriving');
          const entries = ['a.file', 'sub', path.join('sub', 'b.file'), path.join('sub', 'deeper'),
            path.join('sub', 'deeper', 'c.file')];
          for (let i = 0; i < 50; i++) {
            entries.push(path.join('sub', `file-${i}.file`));
          }
          for (const entry of entries) {
            if (path.extname(entry) === '.file') {
              await outputFile(path.join(source, entry), 'Arriving');
            }
          }
          await outputFile(path.join(source, 'ignored', 'd.file'), 'Ignored');
          await outputFile(path.join(source, 'sub', 'ignored.log'), 'Ignored');

          const events = [];
          let watch = await nsfw(inPath, batch => events.push(...batch),
            { debounceMS: DEBOUNCE, excludedPatterns: ['/arriving/ignored/', '*.log'] });
          try {
            await watch.start();
            if (how === 'moved') {
              await fsp.rename(source, target);
            } else {
              await fsp.cp(source, target, { recursive: true });
            }
            await barrier(events, inPath);

            const reported = events.filter(event => event.action !== nsfw.actions.MODIFIED &&
              !(event.directory === inPath && event.file?.startsWith('barrier-')))
              .map(event => `${event.action} ${path.relative(inPath, path.join(event.directory, event.file))}`);
            const expected = ['arriving', ...entries.map(entry => path.join('arriving', entry))]
              .map(entry => `${nsfw.actions.CREATED} ${entry}`);
            assert.deepStrictEqual(reported.sort(), expected.sort());
          } finally {
            await watch.stop();
            watch = null;
          }
        });
    }

    it('does not loop endlessly when watching directories with recursive symlinks', async function () {
      await fsp.mkdir(path.join(workDir, 'test'));
      await fsp.symlink(path.join(workDir, 'test'), path.join(workDir, 'test', 'link'));

      let watch = await nsfw(
        workDir,
        () => {},
        { debounceMS: DEBOUNCE, errorCallback() {} }
      );

      try {
        await watch.start();
        await watch.stop();
      } finally {
        watch = null;
      }
    });
  });

  describe('Pausing', function () {
    it('can pause and resume event reporting', async function () {
      const inPath = path.resolve(workDir, 'test2', 'folder2');
      const events = [];
      let watch = await nsfw(workDir, batch => events.push(...batch), { debounceMS: DEBOUNCE });
      // Once this one reports a change made while paused, the paused watcher has likely had it too: FSEvents only
      // delivers it after its latency, which can be after resume() otherwise.
      const witnessEvents = [];
      const witness = await nsfw(workDir, batch => witnessEvents.push(...batch), { debounceMS: DEBOUNCE });
      const isFor = file => event => event.directory === inPath && (event.file === file || event.newFile === file);
      const reported = (list, file) => list.some(isFor(file));

      try {
        await witness.start();
        await watch.start();
        // A write can raise more than one notification (a creation, then a modification once the file is closed),
        // and the paused watcher's thread can read them later than the witness's. Within the watcher, though, they
        // come in order: if it reports the write, it read it after resume(), and the marker after it. Only an
        // attempt whose marker isn't reported shows pause() dropping events.
        let file;
        for (let attempt = 0; ; ++attempt) {
          file = `paused-${attempt}.file`;
          const marker = `paused-${attempt}.marker`;
          await watch.pause();
          await fsp.writeFile(path.join(inPath, file), 'You will not see me.');
          await fsp.writeFile(path.join(inPath, marker), 'Marker');
          await waitForEvent(witnessEvents, isFor(marker), marker);
          await watch.resume();
          // Whatever the pause held back would come before the barrier.
          await barrier(events, workDir);
          if (!reported(events, marker)) {
            assert.ok(!reported(events, file), `${file} reported: ${JSON.stringify(events)}`);
            break;
          }
          assert.ok(attempt < 4, `Every write made while paused was read after resume: ${JSON.stringify(events)}`);
        }

        await fsp.writeFile(path.join(inPath, file), 'But you will see me.');
        await waitUntil(() => reported(events, file), { message: 'No event after resume' });
      } finally {
        await Promise.all([watch.stop(), witness.stop()]);
        watch = null;
      }
    });
  });

  describe('Errors', function() {
    it('can gracefully recover when the watch folder is deleted', async function() {
      const inPath = path.join(workDir, 'test4');
      let erroredOut = false;
      let resolveEvent;
      const received = new Promise(resolve => { resolveEvent = resolve; });
      let watch = await nsfw(
        inPath,
        events => {
          if (events.some(event => event.file === 'after-recovery')) resolveEvent();
        },
        { debounceMS: DEBOUNCE, errorCallback() { erroredOut = true; } }
      );

      try {
        await watch.start();
        await remove(inPath);
        await waitUntil(() => erroredOut, { message: 'No error' });
        await assert.rejects(watch.stop(), /not running/);
        await fsp.mkdir(inPath);
        await watch.start();
        await fsp.writeFile(path.join(inPath, 'after-recovery'), 'watching again');
        await received;
      } finally {
        await watch.stop();
        watch = null;
      }
    });

    it('reports no error when stopped while changes are coming, and watches again once started', async function() {
      const inPath = path.join(workDir, 'test4');
      const errors = [];
      const events = [];
      let watch = await nsfw(
        inPath,
        batch => events.push(...batch),
        { debounceMS: DEBOUNCE, errorCallback(error) { errors.push(error); } }
      );

      try {
        for (let run = 0; run < 20; run++) {
          await watch.start();
          // Stopping while these changes complete the pending read must neither report an error nor keep the next
          // run from watching.
          const writes = [];
          for (let i = 0; i < 20; i++) {
            writes.push(fsp.writeFile(path.join(inPath, `cancelled-${run}-${i}.file`), 'Cancelled'));
          }
          await Promise.all([watch.stop(), ...writes]);
        }
        await watch.start();
        await barrier(events, inPath);
        assert.deepStrictEqual(errors, []);
      } finally {
        await watch.stop();
        watch = null;
      }
    });

    it('Can pass falsy values to errorCallback', async function() {
      const ok = [undefined, null, 0, '', false];
      const notOk = [1, true, 'a', {}, []];
      await Promise.all([
        ...ok.map(async errorCallback => nsfw(
          workDir,
          () => {},
          { errorCallback }
        )),
        ...notOk.map(async errorCallback => {
          try {
            await nsfw(
              workDir,
              () => {},
              { errorCallback },
            );
          } catch (error) {
            assert.ok(error.message === 'options.errorCallback must be a function.');
            return;
          }
          assert.fail('nsfw must error if errorCallback is not a function nor a falsy value');
        })
      ]);
    });

    it('Report error on rename watched folder', async function() {
      const inPath = path.join(workDir, 'test4');
      const renamed = path.join(workDir, 'test4_renamed');
      let errorOk = false;
      let watch = await nsfw(
        inPath,
        () => { },
        {
          errorCallback: (error) => {
            errorOk = error.message === 'Service shutdown: root path changed (renamed or deleted)';
          }
        }
      );
      try {
        await watch.start();
        await fsp.rename(inPath, renamed);
        await waitUntil(() => errorOk, { message: 'No shutdown error' });
      } finally {
        try {
          await watch.stop();
        } catch (err) { /* do nothing */ }
        watch = null;
      }
    });

    it('reports the first events at once and an error without waiting out debounceMS', async function() {
      const inPath = path.join(workDir, 'test4');
      const debounceMS = 10000;
      const events = [];
      let error = null;
      let watch = await nsfw(inPath, batch => events.push(...batch), {
        debounceMS,
        errorCallback(err) { error = err; }
      });
      try {
        await watch.start();
        await fsp.writeFile(path.join(inPath, 'first.file'), 'First');
        await waitUntil(() => events.some(event => event.file === 'first.file'),
          { timeout: debounceMS / 2, message: 'The first event waited for debounceMS' });
        // A delivery just happened: whatever comes next waits until debounceMS after it, except an error.
        await fsp.rename(inPath, path.join(workDir, 'test4_renamed'));
        await waitUntil(() => error !== null, { timeout: debounceMS / 2, message: 'The error waited for debounceMS' });
        assert.equal(error.message, 'Service shutdown: root path changed (renamed or deleted)');
      } finally {
        await watch.stop().catch(() => {});
        watch = null;
      }
    });

    if (process.platform === 'linux') {
      it('Report error on a directory above the watched folder replaced by another at the same path', async function() {
        const parentPath = path.join(workDir, 'test4');
        const inPath = path.join(parentPath, 'test4_watched');
        let errorOk = false;
        let watch;
        try {
          await fsp.mkdir(inPath);
          watch = await nsfw(inPath, () => {}, {
            errorCallback(error) {
              errorOk = error.message === 'Service shutdown: root path changed (renamed or deleted)';
            }
          });
          await watch.start();
          await fsp.rename(parentPath, path.join(workDir, 'test4_renamed'));
          await fsp.mkdir(inPath, { recursive: true });
          await waitUntil(() => errorOk, { message: 'No shutdown error' });
        } finally {
          await watch.stop().catch(() => {});
          watch = null;
        }
      });
    }

    if (process.platform !== 'win32') {
      it('Report error on delete watched folder', async function() {
        const inPath = path.join(workDir, 'test5');
        let errorMsg = '';
        let watch = await nsfw(
          inPath,
          () => { },
          {
            errorCallback: (error) => {
              errorMsg = error.message;
            }
          }
        );
        try {
          await watch.start();
          await remove(inPath);
          await waitUntil(() => errorMsg !== '', { message: 'No error' });

          assert.equal(errorMsg, 'Service shutdown: root path changed (renamed or deleted)');
        } finally {
          try {
            await watch.stop();
          } catch (err) { /* do nothing */ }
          watch = null;
        }
      });

      it('Report error on rename parent of the watched folder', async function() {
        const parentPath = path.join(workDir, 'test4');
        const renamed = path.join(workDir, 'test4_renamed');
        const inPath = path.join(parentPath, 'test4_watched');
        let errorOk = false;
        let watch;
        try {
          await fsp.mkdir(inPath);
          watch = await nsfw(
            inPath,
            () => { },
            {
              errorCallback: (error) => {
                errorOk = error.message === 'Service shutdown: root path changed (renamed or deleted)';
              }
            }
          );
          await watch.start();
          await fsp.rename(parentPath, renamed);
          await waitUntil(() => errorOk, { message: 'No shutdown error' });
        } finally {
          try {
            await watch.stop();
          } catch (err) { /* do nothing */ }
          watch = null;
        }
      });

      it('Report error on delete parent of the watched folder', async function() {
        const parentPath = path.join(workDir, 'test4');
        const inPath = path.join(parentPath, 'test4_watched');
        let errorOk = false;
        let watch;
        try {
          await fsp.mkdir(inPath);
          watch = await nsfw(
            inPath,
            () => { },
            {
              errorCallback: (error) => {
                errorOk = error.message === 'Service shutdown: root path changed (renamed or deleted)';
              }
            }
          );
          await watch.start();
          await remove(parentPath);
          await waitUntil(() => errorOk, { message: 'No shutdown error' });
        } finally {
          try {
            await watch.stop();
          } catch (err) { /* do nothing */ }
          watch = null;
        }
      });
    }

  });

  describe('Stress', function() {
    const stressRepoPath = path.resolve('nsfw-stress-test');

    beforeEach(function() {
      return execFile('git', ['clone', 'https://github.com/implausible/nsfw-stress-test']);
    });

    it('does not segfault under stress', async function() {
      let count = 0;
      let errorRestart = Promise.resolve();
      let watch = await nsfw(
        stressRepoPath,
        () => { count++; },
        {
          errorCallback() {
            errorRestart = errorRestart.then(() => watch.start());
          }
        }
      );

      try {
        await watch.start();
        await remove(path.join(stressRepoPath, 'folder'));
        await errorRestart;
        assert.ok(count > 0);

        await watch.stop();
        await remove(stressRepoPath);
        await fsp.mkdir(stressRepoPath);

        count = 0;
        errorRestart = Promise.resolve();
        watch = await nsfw(
          stressRepoPath,
          () => { count++; },
          {
            errorCallback() {
              errorRestart = errorRestart.then(() => watch.start());
            }
          }
        );

        await watch.start();
        await execFile('git', ['clone', 'https://github.com/implausible/nsfw-stress-test',
          path.join(stressRepoPath, 'test')]);
        await fsp.stat(path.join(stressRepoPath, 'test'));
        await remove(path.join(stressRepoPath, 'test'));
        await errorRestart;

        assert.ok(count > 0);
      } finally {
        await watch.stop();
        watch = null;
      }
    });

    it('creates and destroys many watchers', async function() {
      for (let i = 0; i < 100; i++) {
        const watcher = await nsfw(stressRepoPath, () => {});
        await watcher.start();
        await watcher.stop();
      }
    });

    afterEach(function() {
      return remove(stressRepoPath);
    });
  });

  describe('Unicode support', function() {
    const watchPath = path.join(workDir, 'は');
    beforeEach(function() {
      return fsp.mkdir(watchPath);
    });

    it('supports watching unicode directories', async function() {
      const file = 'ñ_unicoded_right_in_the.talker';
      let eventFound = false;

      function findEvent(element) {
        if (
          element.action === nsfw.actions.CREATED &&
          element.directory === watchPath &&
          element.file === file
        ) {
          eventFound = true;
        }
      }

      let watch = await nsfw(
        workDir,
        events => events.forEach(findEvent),
        { debounceMS: DEBOUNCE }
      );

      try {
        await watch.start();
        await fsp.writeFile(path.join(watchPath, file), 'Unicode though.');
        await waitUntil(() => eventFound, { message: 'No event' });
      } finally {
        await watch.stop();
        watch = null;
      }
    });
  });

  describe('Excluded paths', function() {
    const debounceMS = 100;
    const eventTimeoutMS = 10000;
    const absenceWindowMS = 200;

    const canonicalPaths = paths => paths.map(value => process.platform === 'win32' ?
      path.toNamespacedPath(value) : value);

    function recordEvents() {
      const events = [];
      const listeners = new Set();

      return {
        events,
        callback(batch) {
          events.push(...batch);
          listeners.forEach(listener => listener());
        },
        async observe(filePath, action, operation, timeoutMS = eventTimeoutMS) {
          const offset = events.length;
          let listener, timer;
          const received = new Promise((resolve, reject) => {
            listener = () => {
              if (events.slice(offset).some(event =>
                matches(event, filePath) && (action === undefined ||
                  (Array.isArray(action) ? action.includes(event.action) : event.action === action)))) {
                resolve();
              }
            };
            listeners.add(listener);
            timer = setTimeout(() => reject(new Error(
              `No action ${action} observed for ${filePath}: ${JSON.stringify(events.slice(offset))}`
            )), timeoutMS);
          });
          try {
            await Promise.all([received, operation()]);
          } finally {
            clearTimeout(timer);
            listeners.delete(listener);
          }
        }
      };
    }

    // Returns a barrier: once the marker file it creates in directory is reported, the events before it have been.
    function markerBarrier(directory, recorder) {
      let marker = 0;
      return async () => {
        const markerPath = path.join(directory, `barrier-${marker++}.file`);
        await recorder.observe(markerPath, undefined, () => fsp.writeFile(markerPath, 'Barrier'));
      };
    }

    function matches(event, filePath) {
      return event.directory === path.dirname(filePath) &&
        (event.file === path.basename(filePath) || event.oldFile === path.basename(filePath));
    }

    // Events for a file with this name anywhere, also under a path the watcher shouldn't use anymore.
    function eventsNamed(events, file) {
      return events.filter(event => [event.file, event.oldFile, event.newFile].includes(file));
    }

    // reportedPath is where the watcher reports inPath's events, when that differs (macOS resolves symlinks).
    // options: more options for the watcher under test.
    async function withWatchers(inPath, excludedPaths, test, reportedPath = inPath, options = {}) {
      const actual = recordEvents();
      const control = recordEvents();
      const watch = await nsfw(inPath, actual.callback, { debounceMS, excludedPaths, ...options });
      const witness = await nsfw(inPath, control.callback, { debounceMS });
      const barrier = markerBarrier(reportedPath, actual);

      async function excluded(filePath, action, operation) {
        const offset = actual.events.length;
        await control.observe(filePath, action, operation);
        await barrier();
        // Absence needs a bounded window even after both watchers have demonstrated progress.
        await new Promise(resolve => setTimeout(resolve, absenceWindowMS));
        assert.deepStrictEqual(actual.events.slice(offset).filter(event => matches(event, filePath)), []);
      }

      try {
        await witness.start();
        await watch.start();
        await barrier();
        await test({ watch, actual, control, barrier, excluded });
      } finally {
        await Promise.all([watch.stop(), witness.stop()]);
      }
    }

    async function appendFile(filePath, contents) {
      // The append itself must land in a later second: a utimes() after it races the FSEvents callback, which can
      // stat the file in between.
      await untilNextSecond(filePath);
      await fsp.appendFile(filePath, contents);
    }

    async function expectReported(actual, directory, file = 'included.file') {
      const filePath = path.join(directory, file);
      await actual.observe(filePath, nsfw.actions.CREATED, () => fsp.writeFile(filePath, 'Included'));
    }

    async function exerciseExcludedFile(filePath, excluded) {
      await excluded(filePath, undefined, () => fsp.writeFile(filePath, 'Excluded.'));
      await excluded(filePath, nsfw.actions.DELETED, () => remove(filePath));
      await excluded(filePath, undefined, () => fsp.writeFile(filePath, 'Excluded again.'));
      await excluded(filePath, undefined, () => fsp.appendFile(filePath, 'Append'));
    }

    it('suppresses every event when the watched root is excluded and resumes after an update', async function() {
      const inPath = path.join(workDir, 'test4');
      const events = [];
      const control = recordEvents();
      const watch = await nsfw(inPath, batch => events.push(...batch), { debounceMS, excludedPaths: [inPath] });
      const witness = await nsfw(inPath, control.callback, { debounceMS });
      try {
        await witness.start();
        await watch.start();
        const hidden = path.join(inPath, 'hidden.file');
        await control.observe(hidden, nsfw.actions.CREATED, () => fsp.writeFile(hidden, 'hidden'));
        await sleep(absenceWindowMS);
        assert.deepStrictEqual(events, []);
        await watch.updateExcludedPaths([]);
        const visible = path.join(inPath, 'visible.file');
        await control.observe(visible, nsfw.actions.CREATED, () => fsp.writeFile(visible, 'visible'));
        await sleep(absenceWindowMS);
        assert.ok(events.some(event => event.file === 'visible.file'));
        await watch.updateExcludedPaths([inPath]);
        events.length = 0;
        await control.observe(hidden, nsfw.actions.DELETED, () => remove(hidden));
        await sleep(absenceWindowMS);
        assert.deepStrictEqual(events, []);
      } finally {
        await Promise.all([watch.stop(), witness.stop()]);
      }
    });

    it('excludes the watched file itself, including deletion and recreation', async function() {
      const filePath = path.join(workDir, 'test0', 'testing0.file');
      const actual = recordEvents();
      const control = recordEvents();
      const watch = await nsfw(filePath, actual.callback,
        { debounceMS: 25, excludedPaths: [filePath] });
      const witness = await nsfw(filePath, control.callback, { debounceMS: 25 });
      try {
        await witness.start();
        await watch.start();
        await control.observe(filePath, nsfw.actions.DELETED, () => remove(filePath));
        await control.observe(filePath, nsfw.actions.CREATED, () => fsp.writeFile(filePath, 'recreated'));
        await sleep(absenceWindowMS);
        assert.deepStrictEqual(actual.events, []);
        await watch.updateExcludedPaths([]);
        await actual.observe(filePath, [nsfw.actions.CREATED, nsfw.actions.MODIFIED],
          () => appendFile(filePath, 'visible'));
        await actual.observe(filePath, nsfw.actions.MODIFIED, () => appendFile(filePath, 'visible again'));
      } finally {
        await Promise.all([watch.stop(), witness.stop()]);
      }
    });

    it('keeps the physical target of a watched symlink followed by dot-dot', async function(t) {
      if (process.platform === 'win32') return t.skip();
      const target = path.join(workDir, 'test5');
      const link = path.join(workDir, 'test4', 'link');
      await fsp.symlink(path.join(target, 'folder5'), link, 'dir');
      const watchPath = link + '/..';
      const excludedPath = target + '/future';
      const watch = await nsfw(watchPath, () => {}, { excludedPaths: [excludedPath] });
      assert.deepStrictEqual(await watch.getExcludedPaths(), [excludedPath]);
      assert.deepStrictEqual(await watch.updateExcludedPaths([watchPath + '/future']), [excludedPath]);
      const actual = recordEvents();
      const reported = process.platform === 'darwin' ? target : watchPath;
      const live = await nsfw(watchPath, actual.callback, { debounceMS });
      try {
        await live.start();
        const file = reported + '/physical.file';
        await fsp.writeFile(file, 'physical');
        // FSEvents can first report what the fixture wrote before the watcher started.
        await waitForEvent(actual.events, event => event.file === 'physical.file' && event.directory === reported,
          'creation of physical.file');
      } finally {
        await live.stop();
      }
    });

    // Replaces fs.promises.realpath() until restore(); replacement(value, realpath) runs for every call.
    function stubRealpath(replacement) {
      const fs = require('fs').promises;
      const realpath = fs.realpath;
      fs.realpath = value => replacement(value, realpath);
      return () => { fs.realpath = realpath; };
    }

    // Holds every fs.promises.realpath() of a path with that basename until release() is called.
    function holdRealpath(basename) {
      let entered, release;
      const held = new Promise(resolve => { entered = resolve; });
      const gate = new Promise(resolve => { release = resolve; });
      const restore = stubRealpath(async (value, realpath) => {
        if (path.basename(value) === basename) {
          entered();
          await gate;
        }
        return realpath(value);
      });
      return {
        held,
        release,
        restore() {
          release();
          restore();
        }
      };
    }

    it('rejects a watched path whose target changes while it is resolved', async function(t) {
      if (process.platform === 'win32') return t.skip();
      const file = path.join(workDir, 'test0', 'testing0.file');
      const target = path.join(workDir, 'test4');
      const alias = path.join(workDir, 'type-alias');
      await fsp.symlink(file, alias, 'file');
      // The alias names the directory while realpath() runs and the file again before anything else reads it, so
      // neither the directory nor the file watcher is what the caller's path names throughout construction.
      const restore = stubRealpath(async (value, realpath) => {
        if (value !== alias) return realpath(value);
        await remove(alias);
        await fsp.symlink(target, alias, 'dir');
        const physical = await realpath(alias);
        await remove(alias);
        await fsp.symlink(file, alias, 'file');
        return physical;
      });
      try {
        await assert.rejects(nsfw(alias, () => {}, { debounceMS }), /^Error: Path must be a valid path/);
      } finally {
        restore();
      }
      const watch = await nsfw(alias, () => {}, { debounceMS });
      await watch.start();
      await watch.stop();
    });

    it('does not invent a collapsed alias for a physical symlink-dot-dot root', async function(t) {
      if (process.platform === 'win32') return t.skip();
      const outside = path.join(workDir, 'test4');
      const target = path.join(workDir, 'test5');
      const link = outside + '/link';
      await fsp.symlink(target + '/folder5', link, 'dir');
      const raw = link + '/..';
      const watch = await nsfw(raw, () => {}, { excludedPaths: [outside, path.join(raw, 'future'), raw + '/future'] });
      assert.deepStrictEqual(await watch.getExcludedPaths(), [target + '/future']);
      await remove(link);
      await fsp.symlink(outside + '/folder4', link, 'dir');
      assert.deepStrictEqual(await watch.updateExcludedPaths([raw + '/future', outside]), [target + '/future']);

      const nestedLink = target + '/link';
      await fsp.mkdir(target + '/folder5/deep');
      await fsp.symlink(target + '/folder5/deep', nestedLink, 'dir');
      const nestedRaw = nestedLink + '/..';
      const nested = await nsfw(nestedRaw, () => {},
        { excludedPaths: [nestedRaw + '/folder5/future', target + '/other'] });
      assert.deepStrictEqual(await nested.getExcludedPaths(), [target + '/folder5/folder5/future']);
    });

    it('snapshots each update input before queued work and recovers from invalid input', async function() {
      const root = path.join(workDir, 'test4');
      const watch = await nsfw(root, () => {});
      const paths = [root + '/first'];
      const first = watch.updateExcludedPaths(paths);
      paths[0] = root + '/second';
      const second = watch.updateExcludedPaths(paths);
      paths[0] = 'invalid';
      await assert.rejects(watch.updateExcludedPaths(paths));
      assert.deepStrictEqual(await Promise.all([first, second]),
        [canonicalPaths([root + '/first']), canonicalPaths([root + '/second'])]);
      assert.deepStrictEqual(await watch.getExcludedPaths(), canonicalPaths([root + '/second']));
      assert.deepStrictEqual(await watch.updateExcludedPaths([]), []);
    });

    it('suppresses metadata events about an excluded macOS root itself', async function(t) {
      if (process.platform !== 'darwin') return t.skip();
      const root = path.join(workDir, 'test4');
      const events = [];
      const control = recordEvents();
      const watch = await nsfw(root, batch => events.push(...batch), { debounceMS, excludedPaths: [root] });
      const witness = await nsfw(root, control.callback, { debounceMS });
      try {
        await witness.start();
        await watch.start();
        await control.observe(root, undefined, () => fsp.chmod(root, 0o700));
        await sleep(absenceWindowMS);
        assert.deepStrictEqual(events, []);
        await watch.updateExcludedPaths([]);
        await control.observe(root, undefined, () => fsp.chmod(root, 0o755));
        for (let i = 0; i < 100 && !events.some(event => event.file === 'test4'); i++) await sleep(50);
        assert.ok(events.some(event => event.file === 'test4'));
      } finally {
        await Promise.all([watch.stop(), witness.stop()]);
      }
    });

    it('updates immediate-child exclusions when watching the Linux filesystem root', async function(t) {
      if (process.platform !== 'linux') return t.skip();
      let child;
      try { child = await fsp.mkdtemp('/nsfw-root-'); } catch (error) {
        if (error.code === 'EACCES' || error.code === 'EROFS') return t.skip();
        throw error;
      }
      const others = (await fsp.readdir('/')).map(name => '/' + name).filter(value => value !== child);
      const actual = recordEvents();
      const control = recordEvents();
      const watch = await nsfw('/', actual.callback, { debounceMS, excludedPaths: others });
      const witness = await nsfw('/', control.callback, { debounceMS, excludedPaths: others });
      try {
        await witness.start();
        await watch.start();
        await watch.updateExcludedPaths([...others, child]);
        const hidden = child + '/hidden';
        await control.observe(hidden, undefined, () => fsp.writeFile(hidden, 'hidden'));
        await sleep(absenceWindowMS);
        assert.ok(!actual.events.some(event => event.file === 'hidden'));
        await watch.updateExcludedPaths(others);
        const visible = child + '/visible';
        await actual.observe(visible, undefined, () => fsp.writeFile(visible, 'visible'));
      } finally {
        await Promise.all([watch.stop(), witness.stop()]);
        await remove(child);
      }
    });

    // Waits for raw.file to move to renamed.file. A move is reported as RENAMED with both directories, or as DELETED
    // in the old directory plus CREATED in the new one when the watcher does not see both halves together (always
    // across directories on Windows, and on Linux when they arrive in different reads). The latter cannot check the
    // newDirectory mapping; canonical-path-spec.js does that for every spelling.
    async function waitForEvents(events, offset, matches) {
      for (let i = 0; i < 200 && !matches(events.slice(offset)); i++) await sleep(50);
      assert.ok(matches(events.slice(offset)), JSON.stringify(events.slice(offset)));
    }

    const waitForEvent = (events, predicate) => waitForEvents(events, 0, batch => batch.some(predicate));

    function waitForMove(events, offset, directory, newDirectory) {
      return waitForEvents(events, offset, after =>
        after.some(event => event.action === nsfw.actions.RENAMED && event.oldFile === 'raw.file' &&
          event.newFile === 'renamed.file' && event.directory === directory && event.newDirectory === newDirectory) ||
        (after.some(event => event.action === nsfw.actions.DELETED && event.file === 'raw.file' &&
          event.directory === directory) &&
          after.some(event => event.action === nsfw.actions.CREATED && event.file === 'renamed.file' &&
            event.directory === newDirectory)));
    }

    it('preserves public event directories for noncanonical watch paths', async function() {
      const inPath = path.join(workDir, 'test4');
      const raw = inPath + path.sep + path.sep + '.' + path.sep;
      const reported = process.platform === 'darwin' ? await fsp.realpath(inPath) :
        process.platform === 'win32' ? path.resolve(raw) + path.sep : raw.slice(0, -1);
      const nestedReported = process.platform === 'win32' ? path.resolve(raw) + path.sep + 'folder4' :
        reported + path.sep + 'folder4';
      const events = [];
      const watch = await nsfw(raw, batch => events.push(...batch), { debounceMS });
      const waitFor = predicate => waitForEvent(events, predicate);
      try {
        await watch.start();
        await fsp.writeFile(path.join(inPath, 'raw.file'), 'raw');
        await waitFor(event => event.file === 'raw.file' && event.directory === reported);
        await fsp.writeFile(path.join(inPath, 'folder4', 'nested.file'), 'nested');
        await waitFor(event => event.file === 'nested.file' &&
          event.directory === nestedReported);
        if (process.platform !== 'darwin') {
          const offset = events.length;
          await fsp.rename(path.join(inPath, 'raw.file'), path.join(inPath, 'folder4', 'renamed.file'));
          await waitForMove(events, offset, reported, nestedReported);
        }
      } finally {
        await watch.stop();
      }
    });

    it('preserves Linux repeated trailing separators for root, nested and moved events', async function(t) {
      if (process.platform !== 'linux') return t.skip();
      const root = path.join(workDir, 'test4');
      const events = [];
      const watch = await nsfw(root + '//', batch => events.push(...batch), { debounceMS });
      const waitFor = predicate => waitForEvent(events, predicate);
      try {
        await watch.start();
        await fsp.writeFile(root + '/raw.file', 'raw');
        await waitFor(event => event.file === 'raw.file' && event.directory === root + '/');
        await fsp.writeFile(root + '/folder4/nested.file', 'nested');
        await waitFor(event => event.file === 'nested.file' && event.directory === root + '//folder4');
        const offset = events.length;
        await fsp.rename(root + '/raw.file', root + '/folder4/renamed.file');
        await waitForMove(events, offset, root + '/', root + '//folder4');
      } finally {
        await watch.stop();
      }
    });

    it('preserves update invocation order while an alternate alias is being resolved', async function(t) {
      if (process.platform === 'win32') return t.skip();
      const inPath = path.join(workDir, 'test4');
      const alias = path.join(workDir, 'alternate');
      await fsp.symlink(inPath, alias, 'dir');
      const watch = await nsfw(inPath, () => {});
      const hold = holdRealpath('alternate');
      try {
        const first = watch.updateExcludedPaths([alias + '/first']);
        await hold.held;
        const second = watch.updateExcludedPaths([inPath + '/second']);
        hold.release();
        assert.deepStrictEqual(await Promise.all([first, second]), [[inPath + '/first'], [inPath + '/second']]);
        assert.deepStrictEqual(await watch.getExcludedPaths(), [inPath + '/second']);
      } finally {
        hold.restore();
      }
    });

    it('applies an update that is still reading the filesystem before later calls', async function(t) {
      if (process.platform === 'win32') return t.skip();
      const inPath = path.join(workDir, 'test4');
      const alias = path.join(workDir, 'alternate');
      await fsp.symlink(inPath, alias, 'dir');
      const actual = recordEvents();
      const control = recordEvents();
      const watch = await nsfw(inPath, actual.callback, { debounceMS });
      const witness = await nsfw(inPath, control.callback, { debounceMS });
      const hold = holdRealpath('alternate');
      try {
        await witness.start();
        // Neither call is awaited before the next one.
        const update = watch.updateExcludedPaths([alias]);
        await hold.held;
        const read = watch.getExcludedPaths();
        const started = watch.start();
        // A start() that overtook the update would be watching by now, without the exclusion.
        await sleep(absenceWindowMS);
        const early = path.join(inPath, 'early.file');
        await control.observe(early, undefined, () => fsp.writeFile(early, 'Early.'));
        await sleep(absenceWindowMS);
        hold.release();
        assert.deepStrictEqual(await read, [inPath]);
        await Promise.all([update, started]);
        const hidden = path.join(inPath, 'hidden.file');
        await control.observe(hidden, undefined, () => fsp.writeFile(hidden, 'Hidden.'));
        await sleep(absenceWindowMS);
        assert.deepStrictEqual(actual.events, []);
      } finally {
        hold.restore();
        await Promise.all([watch.stop(), witness.stop()]);
      }
    });

    it('ignores paths that the filesystem cannot look up', async function(t) {
      if (process.platform === 'win32') return t.skip();
      const inPath = path.join(workDir, 'test4');
      const outside = path.join(workDir, 'test5');
      const blocked = path.join(inPath, 'folder4');
      const outsideBlocked = path.join(outside, 'folder5');
      // realpath() resolves these, but a lookup through a directory that cannot be searched fails, also when the
      // dot-dot comes from a symlink target.
      await fsp.symlink('test5/folder5/../../test4', path.join(workDir, 'through-blocked'), 'dir');
      // Watched as link6/../alias, whose collapsed spelling realpath() also resolves to the watched root.
      await fsp.symlink(path.join(workDir, 'test6', 'folder6'), path.join(workDir, 'link6'), 'dir');
      await fsp.symlink(inPath, path.join(workDir, 'test6', 'alias'), 'dir');
      await fsp.symlink('test5/folder5/../../test4', path.join(workDir, 'alias'), 'dir');
      const unusable = [
        blocked + '/..', blocked + '/../future', outsideBlocked + '/../../test4', outsideBlocked + '/../../test4/x',
        workDir + '/through-blocked', workDir + '/through-blocked/future',
        // An unreadable component is not a nonexistent one: dot-dot must not cancel it back into the root.
        outsideBlocked + '/hidden/../../../test4/future'
      ];
      await fsp.chmod(outsideBlocked, 0);
      await fsp.chmod(blocked, 0);
      try {
        // Permission bits do not apply to a superuser.
        if (await fsp.stat(blocked + '/..').then(() => true, error => error.code !== 'EACCES')) return t.skip();
        const watch = await nsfw(inPath, () => {}, { excludedPaths: unusable });
        assert.deepStrictEqual(await watch.getExcludedPaths(), []);
        assert.deepStrictEqual(await watch.updateExcludedPaths(unusable), []);
        const viaAlias = await nsfw(workDir + '/link6/../alias', () => {},
          { excludedPaths: [workDir + '/alias', workDir + '/alias/future'] });
        assert.deepStrictEqual(await viaAlias.getExcludedPaths(), []);
        for (const watchPath of [outsideBlocked + '/../../test4', workDir + '/through-blocked']) {
          await assert.rejects(nsfw(watchPath, () => {}), /^Error: Path must be a valid path/);
        }
      } finally {
        await fsp.chmod(outsideBlocked, 0o755);
        await fsp.chmod(blocked, 0o755);
      }
    });

    it('ignores nonexistent components before dot-dot on POSIX', async function() {
      const inPath = path.join(workDir, 'test4');
      const value = workDir + path.sep + 'missing' + path.sep + '..' + path.sep + 'test4' + path.sep + 'future';
      const watch = await nsfw(inPath, () => {}, { excludedPaths: [value] });
      const expected = process.platform === 'win32' ? canonicalPaths([path.join(inPath, 'future')]) : [];
      assert.deepStrictEqual(await watch.getExcludedPaths(), expected);
      assert.deepStrictEqual(await watch.updateExcludedPaths([value]), expected);
    });

    it('follows symlinks before dot-dot outside the watched root', async function(t) {
      if (process.platform === 'win32') return t.skip();
      const inPath = path.join(workDir, 'test4');
      const away = path.join(workDir, 'away');
      // away/.. is test5, so away/../test4 names test5/test4 and not the watched root.
      await fsp.symlink(path.join(workDir, 'test5', 'folder5'), away, 'dir');
      const outside = [away + '/../test4', away + '/../test4/future', inPath + '/../away/../test4/future'];
      const actual = recordEvents();
      const watch = await nsfw(inPath, actual.callback, { debounceMS, excludedPaths: outside });
      assert.deepStrictEqual(await watch.getExcludedPaths(), []);
      assert.deepStrictEqual(await watch.updateExcludedPaths(outside), []);
      try {
        await watch.start();
        const reported = path.join(inPath, 'reported.file');
        await actual.observe(reported, undefined, () => fsp.writeFile(reported, 'Reported.'));
      } finally {
        await watch.stop();
      }

      const into = path.join(workDir, 'into');
      // into/.. is the watched root itself, and into is a directory below it.
      await fsp.symlink(path.join(inPath, 'folder4'), into, 'dir');
      const inside = [into + '/../future', into + '/../../test4/other', inPath + '/../into/../later', into + '/child'];
      assert.deepStrictEqual(await watch.updateExcludedPaths(inside),
        [inPath + '/future', inPath + '/other', inPath + '/later', inPath + '/folder4/child']);
    });

    it('follows symlinks before dot-dot below the watched root', async function(t) {
      if (process.platform === 'win32') return t.skip();
      const inPath = path.join(workDir, 'test4');
      const away = path.join(inPath, 'away');
      const near = path.join(inPath, 'near');
      await fsp.symlink(path.join(workDir, 'test5', 'folder5'), away, 'dir');
      await fsp.mkdir(path.join(inPath, 'folder4', 'deep'));
      await fsp.symlink(path.join(inPath, 'folder4', 'deep'), near, 'dir');
      // away/.. is test5, outside the watched root; near/.. is folder4, below it.
      const outside = [away + '/..', away + '/../future', away + '/../test4'];
      const actual = recordEvents();
      const watch = await nsfw(inPath, actual.callback, { debounceMS, excludedPaths: outside });
      assert.deepStrictEqual(await watch.getExcludedPaths(), []);
      assert.deepStrictEqual(await watch.updateExcludedPaths(outside), []);
      try {
        await watch.start();
        const reported = path.join(inPath, 'reported.file');
        await actual.observe(reported, undefined, () => fsp.writeFile(reported, 'Reported.'));
      } finally {
        await watch.stop();
      }
      assert.deepStrictEqual(
        await watch.updateExcludedPaths([near + '/../future', away + '/child', inPath + '/folder4/../later']),
        [inPath + '/folder4/future', away + '/child', inPath + '/later']);
    });

    it('requires a traversable cumulative prefix before keeping lexical children', async function(t) {
      if (process.platform === 'win32') return t.skip();
      const inPath = path.join(workDir, 'test4');
      const dangling = path.join(workDir, 'dangling');
      const loop = path.join(workDir, 'loop');
      const file = path.join(workDir, 'test5', 'testing5.file');
      await fsp.symlink(path.join(workDir, 'nowhere', 'deep'), dangling, 'dir');
      await fsp.symlink(loop, loop, 'dir');
      await fsp.symlink(dangling, path.join(inPath, 'dangling'), 'dir');
      // realpath() resolves the dot-dot in this target, but the lookup through the file fails.
      await fsp.symlink('test5/testing5.file/../../test4', path.join(workDir, 'through-file'), 'dir');
      // Each hop is valid, but more of them than the kernel follows in one lookup make the path fail with ELOOP.
      await fsp.symlink('.', path.join(workDir, 'hop'), 'dir');
      await fsp.symlink(path.join(workDir, 'test5'), path.join(inPath, 'away'), 'dir');
      const unusable = [
        dangling + '/../test4', dangling + '/../test4/future', loop + '/../test4/future',
        file + '/../../test4/future', file + '/missing/../../../test4/future',
        inPath + '/dangling/..', inPath + '/testing4.file/../future',
        workDir + '/through-file', workDir + '/through-file/future',
        workDir + '/hop'.repeat(48) + '/test4', workDir + '/hop'.repeat(48) + '/test4/future',
        workDir + '/missing/..' + '/hop'.repeat(48) + '/test4', inPath + '/folder4/..'.repeat(900),
        // Missing components before dot-dot cannot be traversed, including after symlinks.
        inPath + '/away/missing/../future', inPath + '/dangling/missing/..'
      ];
      const missing = [
        workDir + '/missing/deeper/../../test4/future', inPath + '/../missing/../test4/later',
        inPath + '/missing/deeper/../../other', workDir + '/hop'.repeat(10) + '/test4/hopped'
      ];
      unusable.push(...missing.slice(0, 3), inPath + '/missing/../child');
      const valid = [missing[3], inPath + '/new/child', inPath + '/folder4/../future'];
      await assert.rejects(fsp.stat(workDir + '/hop'.repeat(48) + '/test4'), { code: 'ELOOP' });
      assert.ok((await fsp.stat(workDir + '/hop'.repeat(10) + '/test4')).isDirectory());
      const watch = await nsfw(inPath, () => {}, { excludedPaths: [...unusable, ...valid] });
      assert.deepStrictEqual(await watch.getExcludedPaths(),
        [inPath + '/hopped', inPath + '/new/child', inPath + '/future']);
      assert.deepStrictEqual(await watch.updateExcludedPaths(valid),
        [inPath + '/hopped', inPath + '/new/child', inPath + '/future']);
      assert.deepStrictEqual(await watch.updateExcludedPaths(unusable), []);
      await assert.rejects(nsfw(workDir + '/through-file', () => {}), /^Error: Path must be a valid path/);
    });

    it('keeps exclusions below internal symlinks in the watched namespace', async function(t) {
      if (process.platform === 'win32') return t.skip();
      const inPath = path.join(workDir, 'test4');
      const link = path.join(inPath, 'linked');
      await fsp.symlink(path.join(workDir, 'test5'), link, 'dir');
      const watch = await nsfw(inPath, () => {}, { excludedPaths: [link + '/future'] });
      assert.deepStrictEqual(await watch.getExcludedPaths(), [link + '/future']);
      assert.deepStrictEqual(await watch.updateExcludedPaths([link]), [link]);
    });

    it('accepts future exclusions under the physical root and binds updates to the original target', async function(t) {
      if (process.platform === 'win32') return t.skip();
      const inPath = path.join(workDir, 'test4');
      const link = path.join(workDir, 'root-link');
      await fsp.symlink(inPath, link, 'dir');
      const watch = await nsfw(link, () => {}, { excludedPaths: [inPath + '/future/child'] });
      assert.deepStrictEqual(await watch.getExcludedPaths(), [inPath + '/future/child']);
      await remove(link);
      await fsp.symlink(path.join(workDir, 'test5'), link, 'dir');
      assert.deepStrictEqual(await watch.updateExcludedPaths([link + '/future']), [inPath + '/future']);
      assert.deepStrictEqual(await watch.updateExcludedPaths([workDir + '//./root-link/future']), [inPath + '/future']);
      assert.deepStrictEqual(await watch.updateExcludedPaths([inPath + '/future']), [inPath + '/future']);
    });

    it('uses the macOS volume case rule for future exclusions', async function(t) {
      if (process.platform !== 'darwin') return t.skip();
      const inPath = path.join(workDir, 'test4');
      const variant = inPath.replace('test4', 'TEST4') + '/future';
      const watch = await nsfw(inPath, () => {}, { excludedPaths: [variant] });
      let expected = [];
      try {
        await fsp.stat(path.dirname(variant));
        expected = [inPath + '/future'];
      } catch (error) { /* This spelling is absent on a case-sensitive volume. */ }
      assert.deepStrictEqual(await watch.getExcludedPaths(), expected);
      assert.deepStrictEqual(await watch.updateExcludedPaths([variant]), expected);
    });

    it('can ignore events from excluded paths', async function() {
      const inPath = path.join(workDir, 'test4');
      const excludedPath = path.join(inPath, 'excluded');
      await fsp.mkdir(excludedPath);

      await withWatchers(inPath, [excludedPath], async ({ actual, excluded }) => {
        await exerciseExcludedFile(path.join(excludedPath, 'excluded.file'), excluded);
        const includedFile = path.join(inPath, 'testing4.file');
        await actual.observe(includedFile, nsfw.actions.DELETED, () => remove(includedFile));
      });
    });

    if (process.platform === 'win32') {
      const watchPaths = [
        ['a trailing backslash', inPath => inPath + '\\'],
        ['a trailing slash', inPath => inPath + '/'],
        ['repeated trailing separators', inPath => inPath + '\\\\'],
        ['a namespaced trailing separator', inPath => path.toNamespacedPath(inPath) + '\\'],
        ['a drive root', inPath => path.parse(inPath).root]
      ];
      for (const [description, watchPath] of watchPaths) {
        it(`excludes subdirectories and reports nested directories when watching ${description}`, async function() {
          const inPath = path.join(workDir, 'test4');
          const watchedPath = watchPath(inPath);
          const reportedPath = watchedPath.startsWith('\\\\?\\') ? path.toNamespacedPath(inPath) : inPath;
          const excludedPath = path.join(reportedPath, 'excluded');
          const includedPath = path.join(reportedPath, 'included');
          await Promise.all([fsp.mkdir(excludedPath), fsp.mkdir(includedPath)]);

          await withWatchers(watchedPath, [excludedPath], async ({ watch, actual, excluded }) => {
            await exerciseExcludedFile(path.join(excludedPath, 'excluded.file'), excluded);
            const includedFile = path.join(includedPath, 'included.file');
            await actual.observe(includedFile, nsfw.actions.CREATED, () => fsp.writeFile(includedFile, 'Included'));
            await watch.updateExcludedPaths([]);
            const reincludedFile = path.join(excludedPath, 'reincluded.file');
            await actual.observe(reincludedFile, nsfw.actions.CREATED, () => fsp.writeFile(reincludedFile, 'Included'));
            await watch.updateExcludedPaths([excludedPath]);
            await exerciseExcludedFile(path.join(excludedPath, 'updated.file'), excluded);
          }, includedPath);
        });
      }
    }

    it('can remove an excluded path', async function() {
      const excludedPath = path.join(workDir, 'T1', 'T2');
      await mkdirp(path.join(excludedPath, 'T3'));
      const filePath = path.join(excludedPath, 'newfile.file');

      await withWatchers(workDir, [excludedPath], async ({ watch, actual, barrier, excluded }) => {
        await excluded(filePath, nsfw.actions.CREATED, () => fsp.writeFile(filePath, 'New file.'));
        await watch.updateExcludedPaths([]);
        await actual.observe(filePath, nsfw.actions.MODIFIED, () => appendFile(filePath, 'Now included.'));
        await barrier();
        assert.ok(!actual.events.some(event => matches(event, filePath) && event.action === nsfw.actions.CREATED));
        assert.ok(!actual.events.some(event => matches(event, filePath) && event.action === nsfw.actions.DELETED));
      });
    });

    it('can add an excluded path', async function() {
      const excludedPath = path.join(workDir, 'T1', 'T2');
      await mkdirp(path.join(excludedPath, 'T3'));
      const filePath = path.join(excludedPath, 'newfile.file');

      await withWatchers(workDir, [], async ({ watch, actual, barrier, excluded }) => {
        await actual.observe(filePath, nsfw.actions.CREATED, () => fsp.writeFile(filePath, 'New file.'));
        await actual.observe(filePath, nsfw.actions.MODIFIED, () => appendFile(filePath, 'Still included.'));
        await barrier();
        await watch.updateExcludedPaths([excludedPath]);
        await excluded(filePath, nsfw.actions.MODIFIED, () => appendFile(filePath, 'Now excluded.'));
      });
    });

    it('can modify excluded paths', async function() {
      const inPath = path.join(workDir, 'test4');
      const excludedPath = path.join(inPath, 'excluded');
      const newExcludedPath = path.join(inPath, 'newExcluded');
      await Promise.all([fsp.mkdir(excludedPath), fsp.mkdir(newExcludedPath)]);

      await withWatchers(inPath, [excludedPath], async ({ watch, actual, excluded }) => {
        assert.deepStrictEqual(await watch.getExcludedPaths(), canonicalPaths([excludedPath]));
        await watch.updateExcludedPaths([excludedPath, newExcludedPath]);
        assert.deepStrictEqual(await watch.getExcludedPaths(), canonicalPaths([excludedPath, newExcludedPath]));
        await exerciseExcludedFile(path.join(excludedPath, 'excluded.file'), excluded);
        await exerciseExcludedFile(path.join(newExcludedPath, 'excluded.file'), excluded);
        const includedFile = path.join(inPath, 'testing4.file');
        await actual.observe(includedFile, nsfw.actions.DELETED, () => remove(includedFile));
      });
    });

    it('does not exclude a sibling whose name starts with the excluded name', async function() {
      const inPath = path.join(workDir, 'test4');
      const excludedPath = path.join(inPath, 'foo');
      const siblingPath = path.join(inPath, 'foobar');
      await Promise.all([fsp.mkdir(excludedPath), fsp.mkdir(siblingPath)]);

      await withWatchers(inPath, [excludedPath], async ({ actual, excluded }) => {
        await exerciseExcludedFile(path.join(excludedPath, 'excluded.file'), excluded);
        const siblingFile = path.join(siblingPath, 'sibling.file');
        await actual.observe(siblingFile, nsfw.actions.CREATED, () => fsp.writeFile(siblingFile, 'Sibling.'));
      });
    });

    it('strips trailing separators from excluded paths', async function() {
      const inPath = path.join(workDir, 'test4');
      const excludedPath = path.join(inPath, 'excluded');
      await fsp.mkdir(excludedPath);

      await withWatchers(inPath, [excludedPath + path.sep + path.sep], async ({ watch, actual, excluded }) => {
        assert.deepStrictEqual(await watch.getExcludedPaths(), canonicalPaths([excludedPath]));
        await exerciseExcludedFile(path.join(excludedPath, 'excluded.file'), excluded);
        const includedFile = path.join(inPath, 'included.file');
        await actual.observe(includedFile, nsfw.actions.CREATED, () => fsp.writeFile(includedFile, 'Included.'));

        assert.deepStrictEqual(await watch.updateExcludedPaths([excludedPath + '/' + path.sep]),
          canonicalPaths([excludedPath]));
        assert.deepStrictEqual(await watch.getExcludedPaths(), canonicalPaths([excludedPath]));
      });
    });

    it('excludes paths that are only inside the watched path once `.` and `..` are resolved', async function() {
      const inPath = path.join(workDir, 'test4');
      const excludedPath = path.join(inPath, 'excluded');
      await fsp.mkdir(excludedPath);
      // Raw, since path.join would collapse them.
      const unresolvedPath = [workDir, 'test5', '..', 'test4', '.', 'excluded'].join(path.sep);
      const unresolvedOther = [inPath, 'excluded', '..', 'other'].join(path.sep);
      const otherPath = path.join(inPath, 'other');
      await fsp.mkdir(otherPath);

      await withWatchers(inPath, [unresolvedPath], async ({ watch, actual, excluded }) => {
        assert.deepStrictEqual(await watch.getExcludedPaths(), canonicalPaths([excludedPath]));
        await exerciseExcludedFile(path.join(excludedPath, 'excluded.file'), excluded);

        assert.deepStrictEqual(await watch.updateExcludedPaths([unresolvedOther]), canonicalPaths([otherPath]));
        await exerciseExcludedFile(path.join(otherPath, 'other.file'), excluded);
        const includedFile = path.join(excludedPath, 'included.file');
        await actual.observe(includedFile, nsfw.actions.CREATED, () => fsp.writeFile(includedFile, 'Included.'));
      });
    });

    it('excludes paths under a watched path that goes through a symlink', async function(t) {
      if (process.platform === 'win32') {
        // Creating a symlink needs extra privileges on Windows.
        return t.skip();
      }
      const linkPath = path.join(workDir, 'test4link');
      await fsp.symlink(path.join(workDir, 'test4'), linkPath, 'dir');
      const excludedPath = path.join(linkPath, 'excluded');
      const otherPath = path.join(linkPath, 'other');
      await Promise.all([fsp.mkdir(excludedPath), fsp.mkdir(otherPath)]);
      // macOS reports the events under the real path, here without the link; Linux under the watched path.
      const reportedPath = process.platform === 'darwin' ? await fsp.realpath(linkPath) : linkPath;
      const reported = (directory, file) => path.join(reportedPath, path.relative(linkPath, directory), file);

      await withWatchers(linkPath, [excludedPath], async ({ watch, actual, excluded }) => {
        await exerciseExcludedFile(reported(excludedPath, 'excluded.file'), excluded);
        const includedFile = reported(linkPath, 'included.file');
        await actual.observe(includedFile, nsfw.actions.CREATED, () => fsp.writeFile(includedFile, 'Included.'));
        assert.deepStrictEqual(await watch.getExcludedPaths(), [path.join(workDir, 'test4', 'excluded')]);

        // Also when they change while watching.
        assert.deepStrictEqual(await watch.updateExcludedPaths([otherPath]), [path.join(workDir, 'test4', 'other')]);
        await exerciseExcludedFile(reported(otherPath, 'other.file'), excluded);
        const reincludedFile = reported(excludedPath, 'reincluded.file');
        await actual.observe(reincludedFile, nsfw.actions.CREATED, () => fsp.writeFile(reincludedFile, 'Included.'));
      }, reportedPath);
    });

    it('excludes a directory created after start()', async function() {
      const inPath = path.join(workDir, 'test4');
      // Shares the name of the directory excluded below, one level up.
      const siblingPath = path.join(inPath, 'sub');
      const excludedPath = path.join(inPath, 'created', 'sub');
      await fsp.mkdir(siblingPath);

      await withWatchers(inPath, [], async ({ watch, actual, excluded }) => {
        await actual.observe(excludedPath, undefined, () => mkdirp(excludedPath));
        assert.deepStrictEqual(await watch.updateExcludedPaths([excludedPath]), canonicalPaths([excludedPath]));
        await exerciseExcludedFile(path.join(excludedPath, 'excluded.file'), excluded);
        const siblingFile = path.join(siblingPath, 'sibling.file');
        await actual.observe(siblingFile, nsfw.actions.CREATED, () => fsp.writeFile(siblingFile, 'Sibling.'));
      });
    });

    it('does not watch an excluded directory created after start()', async function() {
      const inPath = path.join(workDir, 'test4');
      const excludedPath = path.join(inPath, 'created-later');

      await withWatchers(inPath, [excludedPath], async ({ control, barrier, excluded }) => {
        await control.observe(excludedPath, undefined, () => fsp.mkdir(excludedPath));
        await barrier();
        await exerciseExcludedFile(path.join(excludedPath, 'excluded.file'), excluded);
      });
    });

    it('does not watch an excluded directory recreated after start()', async function() {
      const inPath = path.join(workDir, 'test4');
      const excludedPath = path.join(inPath, 'recreated');
      await fsp.mkdir(excludedPath);

      await withWatchers(inPath, [excludedPath], async ({ control, barrier, excluded }) => {
        await control.observe(excludedPath, undefined, () => remove(excludedPath));
        await control.observe(excludedPath, undefined, () => fsp.mkdir(excludedPath));
        await barrier();
        await exerciseExcludedFile(path.join(excludedPath, 'excluded.file'), excluded);
      });
    });

    for (const crossParent of [false, true]) {
      it(`respects exclusions when a directory moves ${crossParent ? 'between parents' : 'within a parent'}`,
        async function() {
          const inPath = path.join(workDir, 'test4');
          const sourceParent = path.join(inPath, 'source');
          const targetParent = crossParent ? path.join(inPath, 'target') : sourceParent;
          const source = path.join(sourceParent, 'included');
          const excludedPath = path.join(targetParent, 'excluded');
          const includedAgain = path.join(sourceParent, 'included-again');
          await mkdirp(source);
          await mkdirp(targetParent);

          await withWatchers(inPath, [excludedPath], async ({ actual, control, barrier, excluded }) => {
            await actual.observe(source, undefined, () => fsp.rename(source, excludedPath));
            await barrier();
            await exerciseExcludedFile(path.join(excludedPath, 'excluded.file'), excluded);
            assert.deepStrictEqual(eventsNamed(actual.events, 'excluded.file'), []);

            await control.observe(excludedPath, undefined, () => fsp.rename(excludedPath, includedAgain));
            await barrier();
            const includedFile = path.join(includedAgain, 'included.file');
            await actual.observe(includedFile, nsfw.actions.CREATED, () => fsp.writeFile(includedFile, 'Included'));
          });
        });
    }

    it('respects excluded descendants when their parent moves', async function() {
      const inPath = path.join(workDir, 'test4');
      const source = path.join(inPath, 'source');
      const target = path.join(inPath, 'target');
      const excludedPath = path.join(target, 'child');
      await mkdirp(path.join(source, 'child'));

      await withWatchers(inPath, [excludedPath], async ({ actual, barrier, excluded }) => {
        await actual.observe(source, undefined, () => fsp.rename(source, target));
        await barrier();
        await exerciseExcludedFile(path.join(excludedPath, 'excluded.file'), excluded);
        assert.deepStrictEqual(eventsNamed(actual.events, 'excluded.file'), []);
      });
    });

    it('watches a formerly excluded descendant after its parent moves', async function() {
      const inPath = path.join(workDir, 'test4');
      const source = path.join(inPath, 'source');
      const target = path.join(inPath, 'target');
      const excludedPath = path.join(source, 'child');
      await mkdirp(excludedPath);

      await withWatchers(inPath, [excludedPath], async ({ actual, barrier }) => {
        await actual.observe(source, undefined, () => fsp.rename(source, target));
        await barrier();
        const includedFile = path.join(target, 'child', 'included.file');
        await actual.observe(includedFile, nsfw.actions.CREATED, () => fsp.writeFile(includedFile, 'Included'));
      });
    });

    it('watches an excluded directory moved to another parent', async function() {
      const inPath = path.join(workDir, 'test4');
      const excludedPath = path.join(inPath, 'excluded');
      const target = path.join(inPath, 'target', 'included');
      await mkdirp(excludedPath);
      await mkdirp(path.dirname(target));

      await withWatchers(inPath, [excludedPath], async ({ actual, control, barrier }) => {
        await control.observe(excludedPath, undefined, () => fsp.rename(excludedPath, target));
        await barrier();
        const includedFile = path.join(target, 'included.file');
        await actual.observe(includedFile, nsfw.actions.CREATED, () => fsp.writeFile(includedFile, 'Included'));
      });
    });

    // Windows can't rename a directory over another one.
    if (process.platform !== 'win32') {
      it('watches an excluded directory renamed over an empty directory', async function() {
        const inPath = path.join(workDir, 'test4');
        const excludedPath = path.join(inPath, 'excluded');
        const target = path.join(inPath, 'target');
        await Promise.all([mkdirp(path.join(excludedPath, 'child')), mkdirp(target)]);

        await withWatchers(inPath, [excludedPath], async ({ actual, control, barrier }) => {
          await control.observe(excludedPath, undefined, () => fsp.rename(excludedPath, target));
          await barrier();
          const includedFile = path.join(target, 'child', 'included.file');
          await actual.observe(includedFile, nsfw.actions.CREATED, () => fsp.writeFile(includedFile, 'Included'));
        });
      });

      for (const crossParent of [false, true]) {
        it(`keeps watching a directory renamed over an empty watched directory ${crossParent ?
          'in another parent' : 'in its parent'}, and renamed back`, async function() {
          const inPath = path.join(workDir, 'test4');
          const source = path.join(inPath, 'source');
          const target = path.join(inPath, crossParent ? 'parent' : '', 'target');
          await Promise.all([mkdirp(path.join(source, 'child')), mkdirp(target)]);

          await withWatchers(inPath, [], async ({ actual, barrier }) => {
            await actual.observe(source, undefined, () => fsp.rename(source, target));
            await barrier();
            const targetFile = path.join(target, 'target.file');
            await actual.observe(targetFile, nsfw.actions.CREATED, () => fsp.writeFile(targetFile, 'Target'));
            const childFile = path.join(target, 'child', 'child.file');
            await actual.observe(childFile, nsfw.actions.CREATED, () => fsp.writeFile(childFile, 'Child'));

            await fsp.mkdir(source);
            await barrier();
            await actual.observe(target, undefined, () => fsp.rename(target, source));
            await barrier();
            const sourceFile = path.join(source, 'child', 'source.file');
            await actual.observe(sourceFile, nsfw.actions.CREATED, () => fsp.writeFile(sourceFile, 'Source'));
          });
        });
      }
    }

    // Only a scan watches through a symlink created after start(), so the one to real comes in a directory moved in.
    it('keeps watching a directory seen below one renamed over a removed symlink whose node held it',
      async function(t) {
        if (process.platform !== 'linux') return t.skip();
        const inPath = path.join(workDir, 'test4');
        const outsidePath = path.join(workDir, 'test5');
        const source = path.join(inPath, 'source');
        const target = path.join(inPath, 'target');
        const linked = path.join(outsidePath, 'linked');
        const incoming = path.join(outsidePath, 'incoming');
        const real = path.join(outsidePath, 'real');
        await Promise.all([source, linked, incoming, real].map(directory => mkdirp(directory)));
        await fsp.symlink(real, path.join(incoming, 'held'));
        await fsp.symlink(linked, target);

        await withWatchers(inPath, [], async ({ actual, barrier }) => {
          await actual.observe(path.join(target, 'incoming', 'held'), nsfw.actions.CREATED,
            () => fsp.rename(incoming, path.join(linked, 'incoming')));
          await actual.observe(path.join(source, 'real'), nsfw.actions.CREATED,
            () => fsp.rename(real, path.join(source, 'real')));
          await actual.observe(target, nsfw.actions.DELETED, () => fsp.unlink(target));
          await actual.observe(source, undefined, () => fsp.rename(source, target));
          await barrier();
          const realFile = path.join(target, 'real', 'real.file');
          await actual.observe(realFile, nsfw.actions.CREATED, () => fsp.writeFile(realFile, 'Real'));
        });
      });

    it('stops watching a directory moved into an excluded directory', async function() {
      const inPath = path.join(workDir, 'test4');
      const excludedPath = path.join(inPath, 'excluded');
      const source = path.join(inPath, 'moving');
      const moved = path.join(excludedPath, 'moving');
      await Promise.all([mkdirp(excludedPath), mkdirp(source)]);

      await withWatchers(inPath, [excludedPath], async ({ actual, barrier, excluded }) => {
        await actual.observe(source, undefined, () => fsp.rename(source, moved));
        await barrier();
        await exerciseExcludedFile(path.join(moved, 'excluded.file'), excluded);
        // Nor under the path it had before.
        assert.deepStrictEqual(eventsNamed(actual.events, 'excluded.file'), []);
      });
    });

    it('stops watching a directory moved out of the watched path, and watches it again when it comes back',
      async function() {
        const inPath = path.join(workDir, 'test4');
        const source = path.join(inPath, 'leaving');
        const outside = path.join(workDir, 'test5', 'leaving');
        const back = path.join(inPath, 'back');
        await mkdirp(source);

        await withWatchers(inPath, [], async ({ actual, barrier }) => {
          await actual.observe(source, undefined, () => fsp.rename(source, outside));
          await barrier();
          await fsp.writeFile(path.join(outside, 'outside.file'), 'Outside');
          await barrier();
          await sleep(absenceWindowMS);
          assert.deepStrictEqual(eventsNamed(actual.events, 'outside.file'), []);

          // From outside, so how it's reported differs by platform.
          await fsp.rename(outside, back);
          await barrier();
          const backFile = path.join(back, 'back.file');
          await actual.observe(backFile, nsfw.actions.CREATED, () => fsp.writeFile(backFile, 'Back'));
        });
      });

    if (process.platform === 'linux') {
      const countDirectories = (directory, excludedPaths) => excludedPaths.includes(directory) ? 0 :
        fs.readdirSync(directory, { withFileTypes: true })
          .filter(entry => entry.isDirectory())
          .reduce((count, entry) => count + countDirectories(path.join(directory, entry.name), excludedPaths), 1);

      const readLimit = name => Number(fs.readFileSync(`/proc/sys/fs/inotify/${name}`, 'utf8'));
      const directoryCount = readLimit('max_queued_events') + 1000;

      // The watches of directories that stop being watched are removed a few at a time, since each removal queues an
      // event: removing more than the inotify queue holds at once would overflow it. Hosts with raised limits would
      // take too long to fill.
      describe('with more directories than the inotify queue holds', {
        skip: directoryCount > 66000 || readLimit('max_user_watches') < directoryCount + 5000
      }, function() {
        const inPath = path.join(workDir, 'test4');
        const dist = path.join(inPath, 'dist');
        // The directory scanned last (in alphabetical order), whose watch is the last to be removed.
        const lastScanned = () => path.join(dist,
          Array.from({ length: Math.ceil(directoryCount / 100) }, (unused, i) => `d${i}`).sort().pop(), 'd99');

        beforeEach(function() {
          for (let i = 0; i * 100 < directoryCount; ++i) {
            for (let j = 0; j < 100; ++j) {
              fs.mkdirSync(path.join(dist, `d${i}`, `d${j}`), { recursive: true });
            }
          }
        });

        async function withLargeTreeWatcher(excludedPaths, test) {
          const actual = recordEvents();
          const errors = [];
          const otherWatches = countInotifyWatches();
          const watch = await nsfw(inPath, actual.callback, {
            debounceMS,
            errorCallback: error => errors.push(error),
            excludedPaths
          });
          const barrier = markerBarrier(inPath, actual);

          const watchesConverge = currentExcludedPaths =>
            watchCountReaches(otherWatches + countDirectories(inPath, currentExcludedPaths), 200);

          try {
            await watch.start();
            await watchesConverge(excludedPaths);
            await test({ watch, actual, barrier, watchesConverge });
            assert.deepStrictEqual(errors, []);
          } finally {
            await watch.stop();
          }
        }

        it('keeps watching after moving it away and onto an excluded path', async function() {
          const outside = path.join(workDir, 'test5', 'dist');
          const build = path.join(inPath, 'build');

          await withLargeTreeWatcher([build], async ({ actual, barrier, watchesConverge }) => {
            await fsp.rename(dist, outside);
            await barrier();
            await watchesConverge([build]);
            await fsp.rename(outside, dist);
            await barrier();
            await watchesConverge([build]);
            await fsp.rename(dist, build);
            await barrier();
            await watchesConverge([build]);
            // Stops while the watches of a tree that just moved away are being removed.
            await fsp.rename(build, dist);
            await barrier();
            await actual.observe(dist, nsfw.actions.DELETED, () => fsp.rename(dist, outside));
          });
        });

        it('stops reporting what an excluded tree holds while its watches are still being removed', async function() {
          await withLargeTreeWatcher([], async ({ watch, actual, barrier, watchesConverge }) => {
            await watch.updateExcludedPaths([dist]);
            const filePath = path.join(lastScanned(), 'excluded.file');
            await fsp.writeFile(filePath, 'Excluded');
            await barrier();
            assert.deepStrictEqual(actual.events.filter(event => matches(event, filePath)), []);
            await watchesConverge([dist]);
          });
        });

        it('reports changes in a tree right after it is included again', async function() {
          await withLargeTreeWatcher([], async ({ watch, actual, barrier, watchesConverge }) => {
            // Its watch is still there when the tree is included again. That the events it queued meanwhile are dropped
            // needs the event loop held, see the slow spec.
            const deepPath = lastScanned();
            const existingPath = path.join(deepPath, 'existing.file');
            const excludedPath = path.join(deepPath, 'excluded.file');
            await fsp.writeFile(existingPath, 'Existing');
            await barrier();

            await watch.updateExcludedPaths([dist]);
            await fsp.writeFile(excludedPath, 'Excluded');
            await watch.updateExcludedPaths([]);
            await actual.observe(existingPath, nsfw.actions.DELETED, () => remove(existingPath));
            await barrier();
            assert.deepStrictEqual(actual.events.filter(event => matches(event, excludedPath)), []);
            await watchesConverge([]);
          });
        });

        it('keeps watching after excluding it, and after including it again right away', async function() {
          await withLargeTreeWatcher([], async ({ watch, actual, watchesConverge }) => {
            // Nothing else happens meanwhile, so the event loop only wakes up for the removals.
            await watch.updateExcludedPaths([dist]);
            await watchesConverge([dist]);

            await watch.updateExcludedPaths([]);
            await watch.updateExcludedPaths([dist]);
            await watch.updateExcludedPaths([]);
            const last = Math.ceil(directoryCount / 100) - 1;
            for (const directory of ['d0/d0', `d${Math.floor(last / 2)}/d50`, `d${last}/d99`]) {
              const filePath = path.join(dist, directory, 'included.file');
              await actual.observe(filePath, nsfw.actions.CREATED, () => fsp.writeFile(filePath, 'Included'));
            }
            await watchesConverge([]);
          });
        });
      });

      it('does not watch an excluded directory through a symlink to it', async function() {
        const inPath = path.join(workDir, 'test4');
        // Scanned before the symlink, so the directory is watched at its own path.
        const excludedPath = path.join(inPath, 'a-real');
        await mkdirp(excludedPath);
        await mkdirp(path.join(inPath, 'links'));
        await fsp.symlink(path.join('..', 'a-real'), path.join(inPath, 'links', 'link'));

        await withWatchers(inPath, [], async ({ watch, actual, excluded }) => {
          await watch.updateExcludedPaths([excludedPath]);
          await exerciseExcludedFile(path.join(excludedPath, 'excluded.file'), excluded);
          assert.deepStrictEqual(eventsNamed(actual.events, 'excluded.file'), []);
        });
      });

      it('keeps watching a directory at a symlink scanned before it, with its exclusions', async function() {
        const inPath = path.join(workDir, 'test4');
        const realPath = path.join(inPath, 'z-real');
        const linkPath = path.join(inPath, 'a-link');
        await mkdirp(path.join(realPath, 'sub'));
        await fsp.symlink('z-real', linkPath);

        await withWatchers(inPath, [path.join(linkPath, 'sub')], async ({ actual, excluded }) => {
          const linkFile = path.join(linkPath, 'linked.file');
          await actual.observe(linkFile, nsfw.actions.CREATED,
            () => fsp.writeFile(path.join(realPath, 'linked.file'), 'Linked'));
          await excluded(path.join(linkPath, 'sub', 'excluded.file'), nsfw.actions.CREATED,
            () => fsp.writeFile(path.join(realPath, 'sub', 'excluded.file'), 'Excluded'));
          assert.deepStrictEqual(actual.events.filter(event => event.directory.startsWith(realPath)), []);
          assert.deepStrictEqual(eventsNamed(actual.events, 'excluded.file'), []);
        });
      });

      for (const settleExclusion of [false, true]) {
        const when = settleExclusion ? 'after events settle' : 'right away';
        it(`restores watches through a symlink when a real path is included again ${when}`, async function() {
          const inPath = path.join(workDir, 'test4');
          const realSub = path.join(inPath, 'z-real', 'sub');
          const linkSub = path.join(inPath, 'a-link', 'sub');
          const blocked = path.join(linkSub, 'blocked');
          await mkdirp(path.join(realSub, 'blocked'));
          await fsp.symlink('z-real', path.join(inPath, 'a-link'));

          await withWatchers(inPath, [blocked], async ({ watch, actual, excluded }) => {
            const write = file => fsp.writeFile(path.join(realSub, file), file);
            await actual.observe(path.join(linkSub, 'before.file'), nsfw.actions.CREATED, () => write('before.file'));

            await watch.updateExcludedPaths([blocked, realSub]);
            if (settleExclusion) {
              await excluded(path.join(linkSub, 'hidden.file'), nsfw.actions.CREATED, () => write('hidden.file'));
            } else {
              await write('hidden.file');
            }
            await watch.updateExcludedPaths([blocked]);

            await actual.observe(path.join(linkSub, 'after.file'), nsfw.actions.CREATED, () => write('after.file'));
            await excluded(path.join(blocked, 'blocked.file'), nsfw.actions.CREATED,
              () => write(path.join('blocked', 'blocked.file')));
            assert.deepStrictEqual(eventsNamed(actual.events, 'hidden.file'), []);
            assert.deepStrictEqual(eventsNamed(actual.events, 'blocked.file'), []);
          });
        });
      }

      it('keeps overlapping alias exclusions until the last spelling is removed', async function() {
        const inPath = path.join(workDir, 'test4');
        const realSub = path.join(inPath, 'z-real', 'sub');
        const reportedSub = path.join(inPath, 'a-link', 'sub');
        const alternateSub = path.join(inPath, 'b-link', 'sub');
        await mkdirp(realSub);
        await fsp.symlink('z-real', path.join(inPath, 'a-link'));
        await fsp.symlink('z-real', path.join(inPath, 'b-link'));

        await withWatchers(inPath, [], async ({ watch, actual, excluded }) => {
          for (const [index, retained] of [realSub, alternateSub].entries()) {
            await watch.updateExcludedPaths([alternateSub, realSub]);
            await watch.updateExcludedPaths([retained]);
            const hidden = `hidden-${index}.file`;
            await excluded(path.join(reportedSub, hidden), nsfw.actions.CREATED,
              () => fsp.writeFile(path.join(realSub, hidden), 'Excluded'));

            await watch.updateExcludedPaths([]);
            const visible = `visible-${index}.file`;
            await actual.observe(path.join(reportedSub, visible), nsfw.actions.CREATED,
              () => fsp.writeFile(path.join(realSub, visible), 'Included'));
          }
        });
      });

      it('keeps a descendant excluded through another alias when its ancestor is included again', async function() {
        const inPath = path.join(workDir, 'test4');
        const realSub = path.join(inPath, 'z-real', 'sub');
        const reportedSub = path.join(inPath, 'a-link', 'sub');
        const blocked = path.join(inPath, 'b-link', 'sub', 'blocked');
        await mkdirp(path.join(realSub, 'blocked'));
        await fsp.symlink('z-real', path.join(inPath, 'a-link'));
        await fsp.symlink('z-real', path.join(inPath, 'b-link'));

        await withWatchers(inPath, [], async ({ watch, actual, excluded }) => {
          await watch.updateExcludedPaths([blocked]);
          await watch.updateExcludedPaths([blocked, realSub]);
          await watch.updateExcludedPaths([blocked]);

          await actual.observe(path.join(reportedSub, 'visible.file'), nsfw.actions.CREATED,
            () => fsp.writeFile(path.join(realSub, 'visible.file'), 'Included'));
          await excluded(path.join(reportedSub, 'blocked', 'hidden.file'), nsfw.actions.CREATED,
            () => fsp.writeFile(path.join(realSub, 'blocked', 'hidden.file'), 'Excluded'));

          await watch.updateExcludedPaths([]);
          await actual.observe(path.join(reportedSub, 'blocked', 'restored.file'), nsfw.actions.CREATED,
            () => fsp.writeFile(path.join(realSub, 'blocked', 'restored.file'), 'Included'));
        });
      });

      it('reports what a formerly excluded descendant holds once its parent moves', async function() {
        const inPath = path.join(workDir, 'test4');
        const source = path.join(inPath, 'source');
        const target = path.join(inPath, 'target');
        const excludedPath = path.join(source, 'child');
        await outputFile(path.join(excludedPath, 'nested', 'held.file'), 'Held');

        await withWatchers(inPath, [excludedPath], async ({ actual, barrier, excluded }) => {
          await exerciseExcludedFile(path.join(excludedPath, 'nested', 'excluded.file'), excluded);
          await actual.observe(path.join(target, 'child', 'nested', 'held.file'), nsfw.actions.CREATED,
            () => fsp.rename(source, target));
          await barrier();
          for (const file of ['held.file', 'excluded.file']) {
            assert.deepStrictEqual(eventsNamed(actual.events, file).map(event => event.directory),
              [path.join(target, 'child', 'nested')]);
          }
        });
      });

      describe('through a symlink scanned before the directory it leads to', function() {
        const inPath = path.join(workDir, 'test4');
        // The exclusions are spelled with this one, while the watches are reported under the symlink.
        const realPath = path.join(inPath, 'z-real');
        const linkPath = path.join(inPath, 'a-link');

        beforeEach(async function() {
          await mkdirp(realPath);
          await fsp.symlink('z-real', linkPath);
        });

        it('excludes a directory renamed to an excluded path', async function() {
          await mkdirp(path.join(realPath, 'visible'));
          await mkdirp(path.join(realPath, 'hidden-other'));

          await withWatchers(inPath, [path.join(realPath, 'hidden')], async ({ actual, barrier, excluded }) => {
            await actual.observe(path.join(linkPath, 'visible'), undefined,
              () => fsp.rename(path.join(realPath, 'visible'), path.join(realPath, 'hidden')));
            await barrier();
            await exerciseExcludedFile(path.join(linkPath, 'hidden', 'excluded.file'), excluded);
            assert.deepStrictEqual(eventsNamed(actual.events, 'excluded.file'), []);
            await expectReported(actual, path.join(linkPath, 'hidden-other'));
          });
        });

        it('excludes a descendant when its parent is renamed', async function() {
          await mkdirp(path.join(realPath, 'source', 'blocked'));
          await mkdirp(path.join(realPath, 'source', 'blocked-other'));
          await mkdirp(path.join(realPath, 'target-other', 'blocked'));
          const excludedPath = path.join(realPath, 'target', 'blocked');

          await withWatchers(inPath, [excludedPath], async ({ actual, barrier, excluded }) => {
            await expectReported(actual, path.join(linkPath, 'source', 'blocked'));
            await actual.observe(path.join(linkPath, 'source'), undefined,
              () => fsp.rename(path.join(realPath, 'source'), path.join(realPath, 'target')));
            await barrier();
            await exerciseExcludedFile(path.join(linkPath, 'target', 'blocked', 'excluded.file'), excluded);
            assert.deepStrictEqual(eventsNamed(actual.events, 'excluded.file'), []);
            for (const sibling of [['target'], ['target', 'blocked-other'], ['target-other', 'blocked']]) {
              await expectReported(actual, path.join(linkPath, ...sibling));
            }
          });
        });

        it('watches a descendant again when its parent is renamed away', async function() {
          await mkdirp(path.join(realPath, 'source', 'blocked'));
          const excludedPath = path.join(realPath, 'source', 'blocked');
          const includedPath = path.join(linkPath, 'target', 'blocked');

          await withWatchers(inPath, [excludedPath], async ({ actual, barrier, excluded }) => {
            await exerciseExcludedFile(path.join(linkPath, 'source', 'blocked', 'excluded.file'), excluded);
            // What it holds by then shows up with it.
            await actual.observe(path.join(includedPath, 'excluded.file'), nsfw.actions.CREATED,
              () => fsp.rename(path.join(realPath, 'source'), path.join(realPath, 'target')));
            await barrier();
            await expectReported(actual, includedPath);
            assert.deepStrictEqual(eventsNamed(actual.events, 'excluded.file').map(event => event.directory),
              [includedPath]);
          });
        });

        it('excludes what takes the place of a renamed parent, not what the parent holds then', async function() {
          await mkdirp(path.join(realPath, 'parent'));
          const excludedPath = path.join(realPath, 'parent', 'future');
          const reportedPath = path.join(linkPath, 'parent', 'future');

          await withWatchers(inPath, [excludedPath], async ({ actual, control, barrier, excluded }) => {
            await actual.observe(path.join(linkPath, 'parent'), undefined,
              () => fsp.rename(path.join(realPath, 'parent'), path.join(realPath, 'moved')));
            await barrier();
            await actual.observe(path.join(linkPath, 'moved', 'future'), nsfw.actions.CREATED,
              () => fsp.mkdir(path.join(realPath, 'moved', 'future')));
            await barrier();
            await expectReported(actual, path.join(linkPath, 'moved', 'future'));

            await control.observe(reportedPath, nsfw.actions.CREATED, () => mkdirp(excludedPath));
            await barrier();
            await exerciseExcludedFile(path.join(reportedPath, 'excluded.file'), excluded);
            assert.deepStrictEqual(eventsNamed(actual.events, 'excluded.file'), []);
          });
        });

        for (const fromStart of [true, false]) {
          const when = fromStart ? 'from the first scan' : 'by an update';
          it(`keeps a directory excluded ${when} when it is recreated`, async function() {
            const excludedPath = path.join(realPath, 'sub');
            const reportedPath = path.join(linkPath, 'sub');
            await mkdirp(excludedPath);
            await mkdirp(path.join(realPath, 'sub-other'));

            await withWatchers(inPath, fromStart ? [excludedPath] : [],
              async ({ watch, actual, control, barrier, excluded }) => {
                if (!fromStart) {
                  await expectReported(actual, reportedPath);
                  await watch.updateExcludedPaths([excludedPath]);
                }
                await exerciseExcludedFile(path.join(reportedPath, 'excluded.file'), excluded);

                await control.observe(reportedPath, nsfw.actions.DELETED, () => remove(excludedPath));
                await control.observe(reportedPath, nsfw.actions.CREATED, () => fsp.mkdir(excludedPath));
                await barrier();
                await exerciseExcludedFile(path.join(reportedPath, 'excluded.file'), excluded);
                assert.deepStrictEqual(eventsNamed(actual.events, 'excluded.file'), []);
                await expectReported(actual, path.join(linkPath, 'sub-other'));

                await watch.updateExcludedPaths([]);
                await expectReported(actual, reportedPath);
              });
          });
        }

        it('watches at its own path a directory whose rename leaves the symlink dangling', async function() {
          const otherPath = path.join(inPath, 'z-other');
          await mkdirp(path.join(realPath, 'hidden'));
          await mkdirp(path.join(realPath, 'shown'));

          await withWatchers(inPath, [path.join(realPath, 'hidden')], async ({ actual, barrier, excluded }) => {
            await exerciseExcludedFile(path.join(linkPath, 'hidden', 'excluded.file'), excluded);
            // No excluded path names hidden once its parent has another name: what it holds shows up with it.
            await actual.observe(path.join(otherPath, 'hidden', 'excluded.file'), nsfw.actions.CREATED,
              () => fsp.rename(realPath, otherPath));
            await barrier();
            for (const directory of ['hidden', 'shown']) {
              await expectReported(actual, path.join(otherPath, directory));
            }
            await actual.observe(path.join(otherPath, 'created'), nsfw.actions.CREATED,
              () => fsp.mkdir(path.join(otherPath, 'created')));
            await barrier();
            await expectReported(actual, path.join(otherPath, 'created'));
          });
        });

        for (const repointed of [false, true]) {
          const change = repointed ? 'repointed' : 'removed';
          it(`watches a directory again once the symlink in its excluded path is ${change}`, async function() {
            const aliasPath = path.join(inPath, 'b-link');
            const reportedPath = path.join(linkPath, 'hidden');
            await mkdirp(path.join(realPath, 'hidden'));
            await mkdirp(path.join(inPath, 'm-other'));
            await fsp.symlink('z-real', aliasPath);

            await withWatchers(inPath, [path.join(aliasPath, 'hidden')], async ({ actual, barrier, excluded }) => {
              await exerciseExcludedFile(path.join(reportedPath, 'excluded.file'), excluded);
              // What it holds by then shows up with it.
              await actual.observe(path.join(reportedPath, 'excluded.file'), nsfw.actions.CREATED, async () => {
                await remove(aliasPath);
                if (repointed) {
                  await fsp.symlink('m-other', aliasPath);
                }
              });
              await barrier();
              await expectReported(actual, reportedPath);
            });
          });
        }

        it('reports a directory at the symlink once it leads elsewhere', async function() {
          const otherPath = path.join(inPath, 'm-other');
          await mkdirp(otherPath);

          await withWatchers(inPath, [path.join(realPath, 'hidden')], async ({ actual, barrier }) => {
            await remove(linkPath);
            await fsp.symlink('m-other', linkPath);
            await barrier();
            // Through the symlink, which no longer leads to the directory the excluded path is in.
            await actual.observe(path.join(otherPath, 'hidden'), nsfw.actions.CREATED,
              () => fsp.mkdir(path.join(linkPath, 'hidden')));
            await barrier();
            await actual.observe(path.join(otherPath, 'hidden', 'included.file'), nsfw.actions.CREATED,
              () => fsp.writeFile(path.join(linkPath, 'hidden', 'included.file'), 'Included'));
          });
        });

        it('follows a symlink in an excluded path to the directory it leads to now', async function() {
          const otherPath = path.join(inPath, 'm-other');
          const aliasPath = path.join(inPath, 'b-link');
          await mkdirp(otherPath);
          await fsp.symlink('z-real', aliasPath);

          const excludedPath = path.join(aliasPath, 'hidden');
          await withWatchers(inPath, [excludedPath], async ({ actual, control, barrier, excluded }) => {
            await remove(aliasPath);
            await fsp.symlink('m-other', aliasPath);
            await barrier();

            // Where the symlink led when the path was excluded.
            await actual.observe(path.join(linkPath, 'hidden'), nsfw.actions.CREATED,
              () => fsp.mkdir(path.join(realPath, 'hidden')));
            await barrier();
            await expectReported(actual, path.join(linkPath, 'hidden'));

            await control.observe(path.join(otherPath, 'hidden'), nsfw.actions.CREATED,
              () => fsp.mkdir(path.join(otherPath, 'hidden')));
            await barrier();
            await exerciseExcludedFile(path.join(otherPath, 'hidden', 'excluded.file'), excluded);
            assert.deepStrictEqual(eventsNamed(actual.events, 'excluded.file'), []);
          });
        });

        it('excludes by an absolute symlink when the watched path is given through another one', async function() {
          const givenPath = path.join(workDir, 'test5', 'given');
          await mkdirp(path.join(realPath, 'hidden'));
          await fsp.symlink(inPath, givenPath);
          // Spelled with no symlink, as the watcher has the watched path.
          await fsp.symlink(path.join(await fsp.realpath(inPath), 'z-real'), path.join(inPath, 'b-link'));

          await withWatchers(givenPath, [path.join(givenPath, 'b-link', 'hidden')], async ({ actual, excluded }) => {
            await exerciseExcludedFile(path.join(givenPath, 'a-link', 'hidden', 'excluded.file'), excluded);
            assert.deepStrictEqual(eventsNamed(actual.events, 'excluded.file'), []);
          });
        });

        // Nothing tells when such a path stops leading to the directory, which would stay excluded.
        it('does not exclude by a path that leads to the directory from outside the watched one', async function() {
          const outsideLink = path.join(workDir, 'test5', 'c-link');
          await mkdirp(path.join(realPath, 'hidden'));
          await fsp.symlink(realPath, outsideLink);
          await fsp.symlink(outsideLink, path.join(inPath, 'b-link'));

          await withWatchers(inPath, [path.join(inPath, 'b-link', 'hidden')],
            ({ actual }) => expectReported(actual, path.join(linkPath, 'hidden')));
        });

        const departures = [
          ['renamed onto an excluded path', 'z-new'],
          ['moved out of the watched path', path.join(workDir, 'test5', 'z-real')],
          ['moved into an excluded directory', path.join('excluded', 'z-real')]
        ];
        for (const nested of [false, true]) {
          for (const [departure, destination] of departures) {
            const what = nested ? 'a directory below the one' : 'the directory';
            it(`stops reporting ${what} the symlink leads to once that one is ${departure}`, async function() {
              const excludedPaths = [path.join(inPath, 'z-new'), path.join(inPath, 'excluded')];
              const leftPath = path.resolve(inPath, destination);
              const backPath = path.join(inPath, 'back');
              const held = directory => nested ? path.join(directory, 'sub') : directory;
              await mkdirp(path.join(inPath, 'excluded'));
              await mkdirp(path.join(realPath, 'sub'));
              if (nested) {
                await remove(linkPath);
                await fsp.symlink(path.join('z-real', 'sub'), linkPath);
              }
              const actual = recordEvents();
              const barrier = markerBarrier(inPath, actual);
              const otherWatches = countInotifyWatches();
              const watchesReachTree = () =>
                watchCountReaches(otherWatches + countDirectories(inPath, excludedPaths), 200);
              const watch = await nsfw(inPath, actual.callback, { debounceMS, excludedPaths });
              try {
                await watch.start();
                await actual.observe(path.join(linkPath, 'held.file'), nsfw.actions.CREATED,
                  () => fsp.writeFile(path.join(held(realPath), 'held.file'), 'Held'));

                await fsp.rename(realPath, leftPath);
                await barrier();
                await fsp.writeFile(path.join(held(leftPath), 'left.file'), 'Left');
                // Nor once the symlink is gone.
                await remove(linkPath);
                await fsp.writeFile(path.join(held(leftPath), 'unlinked.file'), 'Unlinked');
                await barrier();
                await sleep(absenceWindowMS);
                assert.deepStrictEqual(eventsNamed(actual.events, 'left.file'), []);
                assert.deepStrictEqual(eventsNamed(actual.events, 'unlinked.file'), []);
                await watchesReachTree();

                await fsp.rename(leftPath, backPath);
                await barrier();
                await expectReported(actual, held(backPath));
                await watchesReachTree();
              } finally {
                await watch.stop();
              }
            });
          }
        }

        // Where the symlink at linkPath leads through another one, or below the directory another one holds.
        const link = path.basename(linkPath);
        const real = path.basename(realPath);
        const relinked = [{
          title: 'a directory below the one another symlink leads to once that one is renamed',
          // b-link holds z-real, and a-link holds sub below it, by the path b-link has.
          directories: [path.join(real, 'sub')],
          links: [[path.join(real, 'sub'), link], [real, 'b-link']],
          held: path.join(real, 'sub'),
          change: () => fsp.rename(realPath, path.join(inPath, 'z-other')),
          reported: path.join('z-other', 'sub')
        }, {
          title: 'a directory below the one another symlink leads to once that one is removed',
          // b-link holds z-real, and a-link holds sub below it, by way of b-link.
          directories: [path.join(real, 'sub')],
          links: [[path.join('b-link', 'sub'), link], [real, 'b-link']],
          held: path.join(real, 'sub'),
          change: () => remove(path.join(inPath, 'b-link')),
          reported: path.join(real, 'sub')
        }, {
          title: 'a directory a symlink leads to through a symlink that moved out',
          // n/x-link holds q, and a-link holds sub below it, by way of n/x-link.
          directories: [path.join('q', 'sub'), 'n'],
          links: [[path.join('..', 'q'), path.join('n', 'x-link')], [path.join('n', 'x-link', 'sub'), link]],
          held: path.join('q', 'sub'),
          change: () => fsp.rename(path.join(inPath, 'n'), path.join(workDir, 'test5', 'n')),
          reported: path.join('q', 'sub')
        }];
        for (const { title, directories, links, held, change, reported } of relinked) {
          it(`reports at its own path ${title}`, async function() {
            await remove(linkPath);
            for (const directory of directories) {
              await mkdirp(path.join(inPath, directory));
            }
            for (const [target, link] of links) {
              await fsp.symlink(target, path.join(inPath, link));
            }

            await withWatchers(inPath, [], async ({ actual, barrier }) => {
              await actual.observe(path.join(linkPath, 'held.file'), nsfw.actions.CREATED,
                () => fsp.writeFile(path.join(inPath, held, 'held.file'), 'Held'));
              await change();
              await barrier();
              await expectReported(actual, path.join(inPath, reported));
              assert.deepStrictEqual(
                eventsNamed(actual.events, 'included.file').filter(event => event.directory.startsWith(linkPath)), []);
            });
          });
        }

        // Nothing tells what changes in a directory that isn't watched.
        describe('by a path through a directory that is excluded itself', function() {
          const unwatchedPath = path.join(inPath, 'b-unwatched');
          const excludedPath = path.join(unwatchedPath, 'b-link', 'hidden');
          const reportedPath = path.join(linkPath, 'hidden');

          beforeEach(async function() {
            await mkdirp(path.join(realPath, 'hidden'));
            await mkdirp(unwatchedPath);
            await fsp.symlink(path.join('..', 'z-real'), path.join(unwatchedPath, 'b-link'));
          });

          it('does not exclude from the start', async function() {
            await withWatchers(inPath, [excludedPath, unwatchedPath],
              ({ actual }) => expectReported(actual, reportedPath));
          });

          it('stops excluding once an update excludes that directory', async function() {
            await withWatchers(inPath, [excludedPath], async ({ watch, actual, excluded }) => {
              await exerciseExcludedFile(path.join(reportedPath, 'excluded.file'), excluded);
              await watch.updateExcludedPaths([excludedPath, unwatchedPath]);
              await expectReported(actual, reportedPath);
            });
          });

          it('excludes once an update stops excluding that directory', async function() {
            await withWatchers(inPath, [excludedPath, unwatchedPath], async ({ watch, actual, excluded }) => {
              await expectReported(actual, reportedPath);
              await watch.updateExcludedPaths([excludedPath]);
              await exerciseExcludedFile(path.join(reportedPath, 'excluded.file'), excluded);
              assert.deepStrictEqual(eventsNamed(actual.events, 'excluded.file'), []);
            });
          });
        });
      });

      describe('through a symlink to a directory scanned before it', function() {
        const inPath = path.join(workDir, 'test4');

        it('watches a directory again once a rename leaves the symlink in its excluded path dangling',
          async function() {
            const oldPath = path.join(inPath, 'a-real', 'old');
            const newPath = path.join(inPath, 'a-real', 'new');
            await mkdirp(path.join(oldPath, 'excluded'));
            await fsp.symlink(path.join('a-real', 'old'), path.join(inPath, 'm-link'));
            const excludedPath = path.join(inPath, 'm-link', 'excluded');

            await withWatchers(inPath, [excludedPath], async ({ actual, barrier, excluded }) => {
              await exerciseExcludedFile(path.join(oldPath, 'excluded', 'excluded.file'), excluded);
              // What it holds by then shows up with it.
              await actual.observe(path.join(newPath, 'excluded', 'excluded.file'), nsfw.actions.CREATED,
                () => fsp.rename(oldPath, newPath));
              await barrier();
              await expectReported(actual, path.join(newPath, 'excluded'));
              assert.deepStrictEqual(eventsNamed(actual.events, 'excluded.file').map(event => event.directory),
                [path.join(newPath, 'excluded')]);
            });
          });

        it('watches at its own path a directory once a rename above it leaves its symlink dangling',
          async function() {
            const oldPath = path.join(inPath, 'b', 'z-real');
            const newPath = path.join(inPath, 'c', 'z-real');
            const linkPath = path.join(inPath, 'a-link');
            await mkdirp(path.join(oldPath, 'x'));
            await mkdirp(path.join(oldPath, 'y'));
            await fsp.symlink(path.join('b', 'z-real'), linkPath);

            await withWatchers(inPath, [path.join(oldPath, 'x')], async ({ actual, barrier, excluded }) => {
              await exerciseExcludedFile(path.join(linkPath, 'x', 'excluded.file'), excluded);
              // No excluded path names x once b has another name: what it holds shows up with it.
              await actual.observe(path.join(newPath, 'x', 'excluded.file'), nsfw.actions.CREATED,
                () => fsp.rename(path.join(inPath, 'b'), path.join(inPath, 'c')));
              await barrier();
              for (const directory of ['x', 'y']) {
                await expectReported(actual, path.join(newPath, directory));
              }
            });
          });

        for (const holder of ['a-link', 'z-real']) {
          const where = holder === 'a-link' ? 'a symlink' : 'its own path';
          it(`watches a directory, held at ${where}, again once a rename hides the symlink in its excluded path`,
            async function() {
              const reportedPath = path.join(inPath, holder, 'x');
              await mkdirp(path.join(inPath, 'z-real', 'x'));
              await mkdirp(path.join(inPath, 'zz-old'));
              if (holder === 'a-link') {
                await fsp.symlink('z-real', path.join(inPath, 'a-link'));
              }
              // Scanned after the directory it leads to, in both cases.
              await fsp.symlink(path.join('..', 'z-real'), path.join(inPath, 'zz-old', 'q-link'));
              const excludedPath = path.join(inPath, 'zz-old', 'q-link', 'x');

              await withWatchers(inPath, [excludedPath], async ({ actual, barrier, excluded }) => {
                await exerciseExcludedFile(path.join(reportedPath, 'excluded.file'), excluded);
                await actual.observe(path.join(reportedPath, 'excluded.file'), nsfw.actions.CREATED,
                  () => fsp.rename(path.join(inPath, 'zz-old'), path.join(inPath, 'zz-new')));
                await barrier();
                await expectReported(actual, reportedPath);
              });
            });
        }

        for (const parent of [[], ['a-real']]) {
          const where = parent.length === 0 ? 'the watched directory' : 'a directory below it';
          it(`excludes a directory once a rename in ${where} makes the symlink in its excluded path lead to it`,
            async function() {
              const oldPath = path.join(inPath, ...parent, 'old');
              const newPath = path.join(inPath, ...parent, 'new');
              await mkdirp(path.join(oldPath, 'excluded'));
              await mkdirp(path.join(oldPath, 'excluded-other'));
              // Dangling until old is renamed.
              await fsp.symlink(path.join(...parent, 'new'), path.join(inPath, 's-link'));
              const excludedPath = path.join(inPath, 's-link', 'excluded');

              await withWatchers(inPath, [excludedPath], async ({ actual, barrier, excluded }) => {
                await expectReported(actual, path.join(oldPath, 'excluded'));
                await actual.observe(oldPath, undefined, () => fsp.rename(oldPath, newPath));
                await barrier();
                await exerciseExcludedFile(path.join(newPath, 'excluded', 'excluded.file'), excluded);
                assert.deepStrictEqual(eventsNamed(actual.events, 'excluded.file'), []);
                await expectReported(actual, path.join(newPath, 'excluded-other'));
              });
            });
        }
      });

      describe('a directory watched at a symlink in a directory scanned before its own', function() {
        const inPath = path.join(workDir, 'test4');
        const holderPath = path.join(inPath, 'node_modules');
        const linkPath = path.join(holderPath, 'pkg');
        const ownPath = path.join(inPath, 'packages', 'pkg');

        beforeEach(async function() {
          await outputFile(path.join(ownPath, 'sub', 'held.file'), 'Held');
          await mkdirp(holderPath);
          await fsp.symlink(path.join('..', 'packages', 'pkg'), linkPath);
        });

        async function expectWatchedAtOwnPathOnly({ actual, barrier }, watchedPath = ownPath, file = 'own.file',
          directories = [watchedPath, path.join(watchedPath, 'sub')]) {
          const offset = actual.events.length;
          for (const directory of directories) {
            await expectReported(actual, directory, file);
          }
          await barrier();
          assert.deepStrictEqual(
            actual.events.slice(offset).filter(event => event.directory.startsWith(holderPath)), []);
        }

        // Its watch isn't dropped: what happens in it right then is reported, and nothing it holds is as created.
        async function expectMovedToOwnPath(watchers, stopWatchingSymlink, watchedPath = ownPath) {
          const heldPath = path.join(watchedPath, 'sub', 'held.file');
          await expectReported(watchers.actual, linkPath);
          await watchers.actual.observe(heldPath, nsfw.actions.DELETED, async () => {
            await stopWatchingSymlink();
            await remove(heldPath);
          });
          await watchers.barrier();
          assert.deepStrictEqual(eventsNamed(watchers.actual.events, 'held.file').map(event => event.action),
            [nsfw.actions.DELETED]);
          await expectWatchedAtOwnPathOnly(watchers, watchedPath);
        }

        const excludable = [['the directory the symlink is in', holderPath], ['the symlink', linkPath]];
        for (const [what, excludedPath] of excludable) {
          it(`is watched at its own path once an update excludes ${what}`, async function() {
            await withWatchers(inPath, [], watchers => expectMovedToOwnPath(watchers,
              () => watchers.watch.updateExcludedPaths([excludedPath])));
          });

          it(`is watched at its own path when ${what} is excluded from the start`, async function() {
            await withWatchers(inPath, [excludedPath], watchers => expectWatchedAtOwnPathOnly(watchers));
          });
        }

        const gone = [
          ['the directory the symlink is in moves out of the watched one',
            () => fsp.rename(holderPath, path.join(workDir, 'test5', 'node_modules'))],
          ['the directory the symlink is in moves where the symlink leads nowhere',
            async () => {
              await mkdirp(path.join(inPath, 'deeper'));
              await fsp.rename(holderPath, path.join(inPath, 'deeper', 'node_modules'));
            }],
          ['the symlink is removed', () => remove(linkPath)],
          ['the symlink is renamed', () => fsp.rename(linkPath, path.join(holderPath, 'renamed'))],
          ['the symlink is repointed', async () => {
            await fsp.symlink(path.join('..', 'packages'), path.join(holderPath, 'repointed'));
            await fsp.rename(path.join(holderPath, 'repointed'), linkPath);
          }],
          ['its rename leaves the symlink dangling',
            () => fsp.rename(ownPath, path.join(inPath, 'packages', 'renamed')),
            path.join(inPath, 'packages', 'renamed')],
          ['a rename above it leaves the symlink dangling',
            () => fsp.rename(path.join(inPath, 'packages'), path.join(inPath, 'renamed')),
            path.join(inPath, 'renamed', 'pkg')]
        ];
        for (const [when, stopWatchingSymlink, watchedPath] of gone) {
          it(`is watched at its own path once ${when}`, async function() {
            await withWatchers(inPath, [],
              watchers => expectMovedToOwnPath(watchers, stopWatchingSymlink, watchedPath));
          });
        }

        it('is watched at its own path once the directory the symlink is in moves into an excluded one',
          async function() {
            const excludedPath = path.join(inPath, 'trash');
            await mkdirp(excludedPath);

            await withWatchers(inPath, [excludedPath], watchers => expectMovedToOwnPath(watchers,
              () => fsp.rename(holderPath, path.join(excludedPath, 'node_modules'))));
          });

        it('stays watched at its own path once the directory the symlink was in is removed and made again',
          async function() {
            await withWatchers(inPath, [], async watchers => {
              await expectMovedToOwnPath(watchers, () => remove(holderPath));
              await mkdirp(holderPath);
              await fsp.symlink(path.join('..', 'packages', 'pkg'), linkPath);
              await watchers.barrier();
              await expectWatchedAtOwnPathOnly(watchers, ownPath, 'again.file');
            });
          });

        const movedPath = path.join(ownPath, 'moved');

        // Reported at the directory's own path, not through the symlink it moved below.
        function expectRenamedIntoOwnPath(events, newDirectory = ownPath) {
          assert.deepStrictEqual(
            eventsNamed(events, 'node_modules').map(({ action, newDirectory, newFile }) =>
              ({ action, newDirectory, newFile })),
            [{ action: nsfw.actions.RENAMED, newDirectory, newFile: 'moved' }]);
        }

        it('keeps watching the directory the symlink is in once it moves into the directory itself', async function() {
          await withWatchers(inPath, [], async watchers => {
            await expectMovedToOwnPath(watchers, () => fsp.rename(holderPath, movedPath));
            await expectReported(watchers.actual, movedPath);
            expectRenamedIntoOwnPath(watchers.actual.events);
          });
        });

        it('keeps watching the directory the symlink is in once it moves into the directory itself, '
          + 'while another symlink scanned before it holds it', async function() {
          await fsp.symlink('node_modules', path.join(inPath, 'a-link'));

          await withWatchers(inPath, [], async watchers => {
            await watchers.actual.observe(holderPath, nsfw.actions.RENAMED, () => fsp.rename(holderPath, movedPath));
            await watchers.barrier();
            await expectWatchedAtOwnPathOnly(watchers, ownPath, 'own.file', [ownPath, movedPath]);
            expectRenamedIntoOwnPath(watchers.actual.events);
          });
        });

        // The directory the symlink is in holds, at another symlink scanned after it, the one the directory's own place
        // is in: both take their own places, that one first.
        it('keeps watching the directory the symlink is in once it moves into the directory itself, '
          + 'which another symlink in it holds the parent of', async function() {
          await fsp.symlink(path.join('..', 'packages'), path.join(holderPath, 'zz-parent'));

          await withWatchers(inPath, [], async watchers => {
            await expectReported(watchers.actual, linkPath);
            await watchers.actual.observe(holderPath, nsfw.actions.RENAMED, () => fsp.rename(holderPath, movedPath));
            await watchers.barrier();
            await expectWatchedAtOwnPathOnly(watchers, ownPath, 'own.file',
              [path.dirname(ownPath), ownPath, path.join(ownPath, 'sub'), movedPath]);
            expectRenamedIntoOwnPath(watchers.actual.events);
          });
        });

        // The symlink it moved below leads out of the watched directory, so it has no own place to take, but the one it
        // is in does: that one takes its own place, and carries it along.
        it('keeps watching the directory the symlink is in once it moves below a symlink out of the watched one, '
          + 'in a directory another symlink in it holds', async function() {
          const outLink = path.join(inPath, 'packages', 'out');
          await fsp.symlink(path.join('..', 'packages'), path.join(holderPath, 'zz-parent'));
          await fsp.symlink(path.join(workDir, 'test5', 'folder5'), outLink);

          await withWatchers(inPath, [], async ({ actual, barrier }) => {
            await actual.observe(holderPath, nsfw.actions.RENAMED,
              () => fsp.rename(holderPath, path.join(workDir, 'test5', 'folder5', 'moved')));
            await barrier();
            await expectReported(actual, path.join(outLink, 'moved'));
            expectRenamedIntoOwnPath(actual.events, outLink);
          });
        });

        // A symlink it holds leads out of the watched directory: what it moved into has no place there.
        it('stops watching the directory the symlink is in once it moves into one a symlink in it leads out to',
          async function() {
            const outsidePath = path.join(workDir, 'test5', 'folder5');
            await fsp.symlink(path.join('..', '..', 'test5', 'folder5'), path.join(holderPath, 'outside'));

            await withWatchers(inPath, [], async ({ actual, barrier }) => {
              await expectReported(actual, path.join(holderPath, 'outside'));
              await actual.observe(holderPath, nsfw.actions.DELETED,
                () => fsp.rename(holderPath, path.join(outsidePath, 'moved')));
              await barrier();
              const offset = actual.events.length;
              await fsp.writeFile(path.join(outsidePath, 'moved', 'outside.file'), 'Outside');
              await barrier();
              await sleep(absenceWindowMS);
              assert.deepStrictEqual(eventsNamed(actual.events.slice(offset), 'outside.file'), []);
              assert.deepStrictEqual(eventsNamed(actual.events, 'node_modules').map(event => event.action),
                [nsfw.actions.DELETED]);
            });
          });

        it('keeps what the path of the symlink excludes once the symlink is removed and made again', async function() {
          await withWatchers(inPath, [path.join(linkPath, 'sub')], async ({ actual, barrier, excluded }) => {
            await remove(linkPath);
            await barrier();
            await fsp.symlink(path.join('..', 'packages', 'pkg'), linkPath);
            await barrier();
            await exerciseExcludedFile(path.join(ownPath, 'sub', 'excluded.file'), excluded);
            assert.deepStrictEqual(eventsNamed(actual.events, 'excluded.file'), []);
            await expectReported(actual, ownPath);
          });
        });

        it('stays unwatched where its own path is excluded, leaves with the symlink or is outside the watched one',
          async function() {
            await mkdirp(path.join(holderPath, 'z-inside'));
            await fsp.symlink('z-inside', path.join(holderPath, 'inside'));
            await fsp.symlink(path.join('..', '..', 'test5', 'folder5'), path.join(holderPath, 'outside'));

            await withWatchers(inPath, [], async ({ watch, actual, excluded }) => {
              await watch.updateExcludedPaths([holderPath, ownPath]);
              for (const link of ['pkg', 'inside', 'outside']) {
                await exerciseExcludedFile(path.join(holderPath, link, 'excluded.file'), excluded);
              }
              assert.deepStrictEqual(eventsNamed(actual.events, 'excluded.file'), []);
            });
          });
      });

      describe('a directory watched at a symlink that leads through another symlink', function() {
        const inPath = path.join(workDir, 'test4');
        const outerPath = path.join(inPath, 'a-outer');
        const innerPath = path.join(inPath, 'b-inner');
        const ownPath = path.join(inPath, 'z-real');

        beforeEach(function() {
          return mkdirp(ownPath);
        });

        async function expectLeftOuterSymlink(
          stopLeading, { watchedPath = ownPath, excludedPaths = [], linkPath = outerPath } = {}
        ) {
          await withWatchers(inPath, excludedPaths, async watchers => {
            const { actual, barrier } = watchers;
            await expectReported(actual, linkPath);
            await stopLeading(watchers);
            await barrier();
            const offset = actual.events.length;
            await expectReported(actual, watchedPath, 'own.file');
            await barrier();
            assert.deepStrictEqual(
              actual.events.slice(offset).filter(event => event.directory.startsWith(linkPath)), []);
          });
        }

        const ways = [
          ['', () => 'b-inner'],
          [', reached out of the watched directory and back', () => path.join('..', 'test4', 'b-inner')],
          [', reached by an absolute path through a symlink to the watched directory', async () => {
            await fsp.symlink('test4', path.join(workDir, 'test4-alias'));
            return path.join(workDir, 'test4-alias', 'b-inner');
          }]
        ];
        for (const [way, outerTarget] of ways) {
          it(`is watched at its own path once the inner symlink is removed${way}`, async function() {
            await fsp.symlink('z-real', innerPath);
            await fsp.symlink(await outerTarget(), outerPath);

            await expectLeftOuterSymlink(() => remove(innerPath));
          });
        }

        it('is watched at its own path once the inner symlink is repointed', async function() {
          await fsp.symlink('z-real', innerPath);
          await fsp.symlink('b-inner', outerPath);
          await mkdirp(path.join(inPath, 'other'));

          await expectLeftOuterSymlink(async () => {
            await fsp.symlink('other', path.join(inPath, 'repointed'));
            await fsp.rename(path.join(inPath, 'repointed'), innerPath);
          });
        });

        it('is watched at its own path once a symlink the inner one is repointed through is removed', async function() {
          await fsp.symlink('z-real', innerPath);
          await fsp.symlink('b-inner', outerPath);
          await mkdirp(path.join(inPath, 'c-links'));
          await fsp.symlink(path.join('..', 'z-real'), path.join(inPath, 'c-links', 'z-link'));

          await expectLeftOuterSymlink(async ({ barrier }) => {
            await fsp.symlink(path.join('c-links', 'z-link'), path.join(inPath, 'repointed'));
            await fsp.rename(path.join(inPath, 'repointed'), innerPath);
            await barrier();
            await remove(path.join(inPath, 'c-links', 'z-link'));
          });
        });

        it('is watched at its own path once an update includes again the directory an inner symlink was removed from',
          async function() {
            const excludedPath = path.join(inPath, 'c-links');
            await mkdirp(excludedPath);
            await fsp.symlink(path.join('..', 'z-real'), path.join(excludedPath, 'b-inner'));
            await fsp.symlink(path.join('c-links', 'b-inner'), outerPath);

            await expectLeftOuterSymlink(async ({ watch }) => {
              await remove(path.join(excludedPath, 'b-inner'));
              await watch.updateExcludedPaths([]);
            }, { excludedPaths: [excludedPath] });
          });

        it('is watched at its own path once the directory the inner symlink is in is renamed', async function() {
          await mkdirp(path.join(inPath, 'c-links'));
          await fsp.symlink(path.join('..', 'z-real'), path.join(inPath, 'c-links', 'b-inner'));
          await fsp.symlink(path.join('c-links', 'b-inner'), outerPath);

          await expectLeftOuterSymlink(() => fsp.rename(path.join(inPath, 'c-links'), path.join(inPath, 'd-links')));
        });

        it('is watched at its own path once an inner symlink to the directory it is in is removed', async function() {
          const targetPath = path.join(inPath, 'sub', 'target');
          await mkdirp(targetPath);
          await fsp.symlink('sub', innerPath);
          await fsp.symlink(path.join('b-inner', 'target'), outerPath);

          await expectLeftOuterSymlink(() => remove(innerPath), { watchedPath: targetPath });
        });

        it('is watched at its own path once an inner symlink outside the watched one is removed', async function() {
          const outsidePath = path.join(workDir, 'test5', 'outside');
          await mkdirp(outsidePath);
          await fsp.symlink(ownPath, path.join(outsidePath, 'b-inner'));
          await fsp.symlink(path.join('..', 'test5', 'outside'), path.join(inPath, 'a-upper'));
          await fsp.symlink(path.join('a-upper', 'b-inner'), outerPath);

          await expectLeftOuterSymlink(() => remove(path.join(outsidePath, 'b-inner')));
        });

        it('is watched at its own path once the symlink above it and then the inner one are removed', async function() {
          const outsidePath = path.join(workDir, 'test5', 'outside');
          const upperPath = path.join(inPath, 'a-upper');
          await mkdirp(outsidePath);
          await fsp.symlink(innerPath, path.join(outsidePath, 'a-outer'));
          await fsp.symlink(path.join('..', 'test5', 'outside'), upperPath);
          await fsp.symlink('z-real', innerPath);

          await expectLeftOuterSymlink(async ({ barrier }) => {
            await remove(upperPath);
            await barrier();
            await remove(innerPath);
          }, { linkPath: path.join(upperPath, 'a-outer') });
        });
      });

      it('removes the watches of directories that move away, including one deleted right after', async function() {
        const inPath = path.join(workDir, 'test4');
        const outsidePath = path.join(workDir, 'test5');
        for (const tree of ['deleted', 'kept']) {
          for (let i = 0; i < 300; ++i) {
            await mkdirp(path.join(inPath, tree, `d${i % 10}`, `d${i}`));
          }
        }
        const actual = recordEvents();
        const otherWatches = countInotifyWatches();
        const watch = await nsfw(inPath, actual.callback, { debounceMS });
        try {
          await watch.start();
          await fsp.rename(path.join(inPath, 'deleted'), path.join(outsidePath, 'deleted'));
          await remove(path.join(outsidePath, 'deleted'));
          await fsp.rename(path.join(inPath, 'kept'), path.join(outsidePath, 'kept'));
          await markerBarrier(inPath, actual)();
          await watchCountReaches(otherWatches + countDirectories(inPath, []), 200);
        } finally {
          await watch.stop();
        }
      });
    }

    it('can include an excluded directory created after start()', async function() {
      const inPath = path.join(workDir, 'test4');
      const excludedPath = path.join(inPath, 'created-later');

      await withWatchers(inPath, [excludedPath], async ({ watch, actual, control, barrier, excluded }) => {
        await control.observe(excludedPath, undefined, () => fsp.mkdir(excludedPath));
        await barrier();
        const filePath = path.join(excludedPath, 'included.file');
        await excluded(filePath, nsfw.actions.CREATED, () => fsp.writeFile(filePath, 'Excluded'));
        await watch.updateExcludedPaths([]);
        await actual.observe(filePath, nsfw.actions.MODIFIED, () => appendFile(filePath, 'Included'));
      });
    });

    it('rejects relative excluded paths and ignores the ones outside the watched directory', async function () {
      const inPath = path.join(workDir, 'test4');
      const validPath = path.join(inPath, 'excluded');
      const relativePaths = ['excluded', path.join('test4', 'excluded')];
      const outsidePaths = [
        path.join(workDir, 'test4bar'),
        path.join(workDir, 'test5'),
        // Raw, since path.join would collapse it.
        `${inPath}${path.sep}..${path.sep}test5`,
        workDir
      ];
      if (process.platform === 'win32') {
        outsidePaths.push(`${inPath[0].toUpperCase() === 'Z' ? 'Y' : 'Z'}:\\test4\\excluded`);
      }

      for (const relativePath of relativePaths) {
        await assert.rejects(
          () => nsfw(inPath, () => {}, { excludedPaths: [validPath, relativePath] }),
          /^Error: Excluded path must be an absolute path\.$/,
          `accepted ${relativePath} at construction`
        );
      }
      await assert.rejects(() => nsfw(inPath, () => {}, { excludedPaths: validPath }), TypeError);
      await assert.rejects(() => nsfw(inPath, () => {}, { excludedPaths: [validPath, 1] }), TypeError);

      const watch = await nsfw(inPath, () => {}, { excludedPaths: [...outsidePaths, validPath] });
      assert.deepEqual(await watch.getExcludedPaths(), canonicalPaths([validPath]));
      for (const relativePath of relativePaths) {
        await assert.rejects(
          () => watch.updateExcludedPaths([relativePath]),
          /^Error: Excluded path must be an absolute path\.$/,
          `accepted ${relativePath} on update`
        );
      }
      await assert.rejects(() => watch.updateExcludedPaths(validPath), TypeError);
      await assert.rejects(() => watch.updateExcludedPaths([validPath, 1]), TypeError);
      assert.deepEqual(await watch.getExcludedPaths(), canonicalPaths([validPath]));

      assert.deepEqual(await watch.updateExcludedPaths(outsidePaths), canonicalPaths([]));
      assert.deepEqual(await watch.getExcludedPaths(), canonicalPaths([]));

      const validPaths = [inPath, validPath, path.join(inPath, '..excluded')];
      const effectivePaths = [...validPaths];
      if (process.platform === 'win32') {
        // Either form counts as inside, and it is kept spelled like the watched path.
        validPaths.push(path.toNamespacedPath(validPath));
        effectivePaths.push(validPath);
      }
      const mixedPaths = [outsidePaths[0], ...validPaths, outsidePaths[1]];
      assert.deepEqual(await watch.updateExcludedPaths(mixedPaths), canonicalPaths(effectivePaths));
      assert.deepEqual(await watch.getExcludedPaths(), canonicalPaths(effectivePaths));
      const constructed = await nsfw(inPath, () => {}, { excludedPaths: mixedPaths });
      assert.deepEqual(await constructed.getExcludedPaths(), canonicalPaths(effectivePaths));
    });

    it('keeps reporting events when the excluded paths are outside the watched directory', async function() {
      const inPath = path.join(workDir, 'proj');
      await fsp.mkdir(inPath);
      const outsidePaths = [
        // A linked worktree's git directory, under its main repository next to the watched one.
        path.join(workDir, 'proj-main', '.git', 'worktrees', 'proj', 'lfs', 'tmp'),
        // An ancestor would match every event in the native watchers.
        workDir,
        // Shares the watched path as a prefix.
        `${inPath}-sibling`
      ];

      await withWatchers(inPath, outsidePaths, async ({ watch, actual }) => {
        assert.deepStrictEqual(await watch.getExcludedPaths(), canonicalPaths([]));
        const createdFile = path.join(inPath, 'created.file');
        await actual.observe(createdFile, nsfw.actions.CREATED, () => fsp.writeFile(createdFile, 'Created.'));

        assert.deepStrictEqual(await watch.updateExcludedPaths(outsidePaths), canonicalPaths([]));
        const updatedFile = path.join(inPath, 'updated.file');
        await actual.observe(updatedFile, nsfw.actions.CREATED, () => fsp.writeFile(updatedFile, 'Updated.'));
      });
    });

    it('resolves each updateExcludedPaths() call with the paths it set', async function () {
      const inPath = path.join(workDir, 'test4');
      const first = [path.join(inPath, 'first')];
      const second = [path.join(inPath, 'second'), path.join(inPath, 'third')];

      const watch = await nsfw(inPath, () => {});
      await watch.start();
      try {
        const results = await Promise.all([watch.updateExcludedPaths(first), watch.updateExcludedPaths(second)]);
        assert.deepEqual(results, [canonicalPaths(first), canonicalPaths(second)]);
        assert.deepEqual(await watch.getExcludedPaths(), canonicalPaths(second));
      } finally {
        await watch.stop();
      }
    });

    describe('by patterns and ignore files', function() {
      const inPath = path.join(workDir, 'test4');
      const isLinux = process.platform === 'linux';

      // A file written over: macOS can report it as created, as FSEvents merges the changes of a path.
      const CHANGED = [nsfw.actions.CREATED, nsfw.actions.MODIFIED];

      // The most inotify watches above before this process held while action ran, sampled every 10 ms.
      async function peakWatchesDuring(before, action) {
        let most = 0;
        const sampling = setInterval(() => { most = Math.max(most, countInotifyWatches() - before); }, 10);
        try {
          await action();
        } finally {
          clearInterval(sampling);
        }
        return most;
      }

      // Creates files in directory until one is reported: the Linux watcher only watches a directory again once the
      // ignore files stayed unchanged for a moment.
      async function eventuallyReported(recorder, directory) {
        for (let attempt = 0; ; attempt++) {
          const filePath = path.join(directory, `attempt-${attempt}.file`);
          try {
            await recorder.observe(filePath, nsfw.actions.CREATED, () => fsp.writeFile(filePath, 'Included'), 1000);
            return;
          } catch (error) {
            if (attempt === 9) throw error;
          }
        }
      }

      // A repository of its own, so the ignore files of the one the tests run in don't apply.
      async function makeRepository(gitignore) {
        await fsp.mkdir(path.join(inPath, '.git'));
        await fsp.writeFile(path.join(inPath, '.gitignore'), gitignore);
      }

      // In a process of its own: the warning is emitted once per process.
      async function deprecationWarnings(usesExcludedPaths) {
        const script = `
          const nsfw = require(${JSON.stringify(path.resolve(__dirname, '../dist'))});
          const codes = [];
          process.on('warning', warning => codes.push(warning.code));
          (async () => {
            const options = ${usesExcludedPaths} ?
              { excludedPaths: [${JSON.stringify(path.join(inPath, 'build'))}] } : { excludedPatterns: ['/build'] };
            const watcher = await nsfw(${JSON.stringify(inPath)}, () => {}, options);
            if (${usesExcludedPaths}) {
              await watcher.updateExcludedPaths([]);
              await watcher.getExcludedPaths();
            }
            await watcher.getExcludedPatterns();
            setImmediate(() => console.log(JSON.stringify(codes)));
          })();
        `;
        const { stdout } = await execFile(process.execPath, ['-e', script]);
        return JSON.parse(stdout);
      }

      it('warns once that excludedPaths is deprecated, and not for excludedPatterns', async function() {
        assert.deepStrictEqual(await deprecationWarnings(true), ['NSFW_EXCLUDED_PATHS']);
        assert.deepStrictEqual(await deprecationWarnings(false), []);
      });

      it('rejects filter options of the wrong type', async function() {
        await assert.rejects(() => nsfw(inPath, () => {}, { excludedPatterns: '*.log' }),
          /^TypeError: excludedPatterns must be an array of strings/);
        await assert.rejects(() => nsfw(inPath, () => {}, { excludedPatterns: [1] }),
          /^TypeError: excludedPatterns must be an array of strings/);
        await assert.rejects(() => nsfw(inPath, () => {}, { respectGitignore: 'yes' }),
          /^TypeError: respectGitignore must be a boolean/);
      });

      it('excludes what glob patterns match', async function() {
        const nested = path.join(inPath, 'nested', 'deeper');
        await mkdirp(nested);
        await mkdirp(path.join(inPath, 'build'));
        const options = { excludedPatterns: ['*.log', 'build/', '/top.file', 'nested/**/*.tmp'] };

        await withWatchers(inPath, [], async ({ actual, excluded }) => {
          await exerciseExcludedFile(path.join(inPath, 'root.log'), excluded);
          await exerciseExcludedFile(path.join(nested, 'nested.log'), excluded);
          await exerciseExcludedFile(path.join(inPath, 'build', 'output.file'), excluded);
          await exerciseExcludedFile(path.join(inPath, 'top.file'), excluded);
          await exerciseExcludedFile(path.join(nested, 'deep.tmp'), excluded);
          await expectReported(actual, path.join(inPath, 'nested'), 'top.file');
          await expectReported(actual, inPath, 'root.tmp');
        }, inPath, options);
      });

      it('includes again what a negated pattern matches', async function() {
        const options = { excludedPatterns: ['*.log', '!keep.log'] };
        await withWatchers(inPath, [], async ({ actual, excluded }) => {
          await exerciseExcludedFile(path.join(inPath, 'other.log'), excluded);
          await expectReported(actual, inPath, 'keep.log');
        }, inPath, options);
      });

      it('reports a rename across the boundary of a pattern as a creation or a removal', async function() {
        const included = path.join(inPath, 'renamed.file');
        const ignored = path.join(inPath, 'renamed.tmp');
        await fsp.writeFile(included, 'Renamed');

        await withWatchers(inPath, [], async ({ actual, barrier }) => {
          await actual.observe(included, nsfw.actions.DELETED, () => fsp.rename(included, ignored));
          await actual.observe(included, nsfw.actions.CREATED, () => fsp.rename(ignored, included));
          await barrier();
          assert.deepStrictEqual(eventsNamed(actual.events, 'renamed.tmp'), []);
        }, inPath, { excludedPatterns: ['*.tmp'] });
      });

      it('replaces the patterns while watching, also from none', async function() {
        const directory = path.join(inPath, 'generated');
        await mkdirp(directory);

        await withWatchers(inPath, [], async ({ watch, actual, barrier, excluded }) => {
          await expectReported(actual, directory, 'before.file');
          assert.deepStrictEqual(await watch.getExcludedPatterns(), []);
          assert.deepStrictEqual(await watch.updateExcludedPatterns(['generated/', '*.tmp']), ['generated/', '*.tmp']);
          assert.deepStrictEqual(await watch.getExcludedPatterns(), ['generated/', '*.tmp']);
          await barrier();
          await exerciseExcludedFile(path.join(directory, 'during.file'), excluded);
          await exerciseExcludedFile(path.join(inPath, 'during.tmp'), excluded);
          await watch.updateExcludedPatterns(['*.tmp']);
          await expectReported(actual, directory, 'after.file');
          await exerciseExcludedFile(path.join(inPath, 'after.tmp'), excluded);
          await watch.updateExcludedPatterns([]);
          await expectReported(actual, inPath, 'last.tmp');
        }, inPath);
      });

      it('rejects patterns of the wrong type in an update and keeps the current ones', async function() {
        const watch = await nsfw(inPath, () => {}, { excludedPatterns: ['*.tmp'] });
        await assert.rejects(() => watch.updateExcludedPatterns('*.log'),
          /^TypeError: excludedPatterns must be an array of strings/);
        await assert.rejects(() => watch.updateExcludedPatterns([null]),
          /^TypeError: excludedPatterns must be an array of strings/);
        assert.deepStrictEqual(await watch.getExcludedPatterns(), ['*.tmp']);
      });

      it('resolves each update with the patterns it set and keeps the last one for the next start', async function() {
        const watch = await nsfw(inPath, () => {}, { debounceMS });
        const input = ['a'];
        const results = await Promise.all([
          watch.updateExcludedPatterns(input),
          watch.updateExcludedPatterns(['b']),
          watch.updateExcludedPatterns(['*.tmp'])
        ]);
        input.push('changed');
        assert.deepStrictEqual(results, [['a'], ['b'], ['*.tmp']]);
        assert.deepStrictEqual(await watch.getExcludedPatterns(), ['*.tmp']);

        const recorder = recordEvents();
        const watching = await nsfw(inPath, recorder.callback, { debounceMS });
        await watching.updateExcludedPatterns(['*.tmp']);
        await watching.start();
        await watching.stop();
        // Kept across a restart too.
        await watching.start();
        try {
          const barrier = markerBarrier(inPath, recorder);
          await barrier();
          await fsp.writeFile(path.join(inPath, 'stopped.tmp'), 'Excluded');
          await barrier();
          assert.deepStrictEqual(eventsNamed(recorder.events, 'stopped.tmp'), []);
        } finally {
          await watching.stop();
        }
      });

      it('keeps the order of pattern and path updates', async function() {
        const excludedPath = path.join(inPath, 'excluded');
        const watch = await nsfw(inPath, () => {}, { debounceMS });
        await watch.start();
        try {
          const calls = await Promise.all([
            watch.updateExcludedPaths([excludedPath]),
            watch.updateExcludedPatterns(['*.tmp']),
            watch.getExcludedPatterns(),
            watch.getExcludedPaths()
          ]);
          assert.deepStrictEqual(calls.slice(1), [['*.tmp'], ['*.tmp'], canonicalPaths([excludedPath])]);
        } finally {
          await watch.stop();
        }
      });

      it('excludes what the ignore files of the repository ignore', async function() {
        await makeRepository('*.tmp\nignored/\n');
        await mkdirp(path.join(inPath, 'ignored'));
        await mkdirp(path.join(inPath, 'sub'));
        await fsp.writeFile(path.join(inPath, 'sub', '.gitignore'), 'local.file\n!keep.tmp\n');

        await withWatchers(inPath, [], async ({ actual, excluded }) => {
          await exerciseExcludedFile(path.join(inPath, 'root.tmp'), excluded);
          await exerciseExcludedFile(path.join(inPath, 'ignored', 'inside.file'), excluded);
          await exerciseExcludedFile(path.join(inPath, 'sub', 'local.file'), excluded);
          await expectReported(actual, path.join(inPath, 'sub'), 'keep.tmp');
          // A new file, which macOS may report as modified, see CHANGED.
          const topLevel = path.join(inPath, 'local.file');
          await actual.observe(topLevel, CHANGED, () => fsp.writeFile(topLevel, 'Included'));
        }, inPath, { respectGitignore: true });
      });

      it('lets the patterns take precedence over the ignore files', async function() {
        await makeRepository('*.tmp\n');
        const options = { respectGitignore: true, excludedPatterns: ['!wanted.tmp', '*.data'] };
        await withWatchers(inPath, [], async ({ actual, excluded }) => {
          await exerciseExcludedFile(path.join(inPath, 'other.tmp'), excluded);
          await exerciseExcludedFile(path.join(inPath, 'other.data'), excluded);
          await expectReported(actual, inPath, 'wanted.tmp');
        }, inPath, options);
      });

      it('follows the changes of an ignore file', async function() {
        await makeRepository('');
        const changing = path.join(inPath, 'changing');
        await mkdirp(changing);
        const gitignore = path.join(inPath, '.gitignore');

        await withWatchers(inPath, [], async ({ actual, barrier, excluded }) => {
          await expectReported(actual, changing, 'before.file');
          await actual.observe(gitignore, CHANGED, () => fsp.writeFile(gitignore, 'changing/\n'));
          await barrier();
          await exerciseExcludedFile(path.join(changing, 'during.file'), excluded);
          await actual.observe(gitignore, nsfw.actions.DELETED, () => remove(gitignore));
          await barrier();
          await eventuallyReported(actual, changing);
        }, inPath, { respectGitignore: true });
      });

      it('never lets the ignore files exclude the .git directory or what it holds', async function() {
        await makeRepository('.*\n!.gitignore\nlogs/\n*.lock\n');
        const head = path.join(inPath, '.git', 'HEAD');
        const lock = path.join(inPath, '.git', 'index.lock');
        await mkdirp(path.join(inPath, '.git', 'logs'));
        const log = path.join(inPath, '.git', 'logs', 'HEAD');
        await withWatchers(inPath, [], async ({ actual, excluded }) => {
          await actual.observe(head, nsfw.actions.CREATED, () => fsp.writeFile(head, 'ref: refs/heads/main\n'));
          await actual.observe(lock, nsfw.actions.CREATED, () => fsp.writeFile(lock, ''));
          await actual.observe(log, nsfw.actions.CREATED, () => fsp.writeFile(log, ''));
          await exerciseExcludedFile(path.join(inPath, '.hidden'), excluded);
        }, inPath, { respectGitignore: true });
      });

      it('reads again a nested ignore file that changed while its directory was ignored', async function() {
        await makeRepository('');
        const nested = path.join(inPath, 'nested');
        await mkdirp(path.join(nested, 'sub'));
        await fsp.writeFile(path.join(nested, '.gitignore'), 'sub/\n');
        const gitignore = path.join(inPath, '.gitignore');

        await withWatchers(inPath, [], async ({ actual, barrier, excluded }) => {
          await exerciseExcludedFile(path.join(nested, 'sub', 'first.file'), excluded);
          const watchesBefore = isLinux ? countInotifyWatches() : 0;
          await actual.observe(gitignore, CHANGED, () => fsp.writeFile(gitignore, 'nested/\n'));
          await barrier();
          if (isLinux) {
            // nested is unwatched, so the change to its own .gitignore goes unseen.
            await watchCountReaches(watchesBefore - 1);
          }
          await fsp.writeFile(path.join(nested, '.gitignore'), '');
          await actual.observe(gitignore, CHANGED, () => fsp.writeFile(gitignore, ''));
          await barrier();
          await eventuallyReported(actual, path.join(nested, 'sub'));
        }, inPath, { respectGitignore: true });
      });

      it('does not report the removal of a directory a pattern for directories excludes', async function() {
        const ignored = path.join(inPath, 'build');
        await mkdirp(path.join(ignored, 'a'));
        await fsp.writeFile(path.join(ignored, 'a', 'output.file'), 'Output');
        await withWatchers(inPath, [], async ({ excluded }) => {
          await excluded(ignored, nsfw.actions.DELETED, () => remove(ignored));
        }, inPath, { excludedPatterns: ['build/'] });
      });

      if (process.platform !== 'win32') {
        it('reports a symlink to an excluded directory, but nothing done through it', async function() {
          const ignored = path.join(inPath, 'build');
          const link = path.join(inPath, 'current');
          await mkdirp(ignored);
          await withWatchers(inPath, [], async ({ actual, excluded }) => {
            await actual.observe(link, nsfw.actions.CREATED, () => fsp.symlink(ignored, link));
            await excluded(path.join(ignored, 'output.file'), undefined,
              () => fsp.writeFile(path.join(link, 'output.file'), 'Output'));
            await actual.observe(link, nsfw.actions.DELETED, () => remove(link));
            assert.deepStrictEqual(eventsNamed(actual.events, 'output.file'), []);
          }, inPath, { excludedPatterns: ['build/'] });
        });

        it('reports a symlink to a file named like a pattern for directories, its removal too', async function() {
          const target = path.join(inPath, 'target.file');
          const link = path.join(inPath, 'build');
          await fsp.writeFile(target, 'A file');
          await withWatchers(inPath, [], async ({ actual }) => {
            await actual.observe(link, nsfw.actions.CREATED, () => fsp.symlink(target, link));
            await actual.observe(link, nsfw.actions.DELETED, () => remove(link));
          }, inPath, { excludedPatterns: ['build/'] });
        });

        // macOS coalesces the events of one path, so whether it tells a replacement apart varies.
        (isLinux ? it : it.skip)(
          'reports the removal of a symlink to a file that a symlink to a directory was renamed over',
          async function() {
            const file = path.join(inPath, 'target.file');
            const directory = path.join(inPath, 'store');
            const link = path.join(inPath, 'build');
            const next = path.join(inPath, 'next');
            await fsp.writeFile(file, 'A file');
            await mkdirp(directory);
            await withWatchers(inPath, [], async ({ actual }) => {
              await actual.observe(link, nsfw.actions.CREATED, () => fsp.symlink(file, link));
              await actual.observe(link, nsfw.actions.DELETED, async () => {
                await fsp.symlink(directory, next);
                await fsp.rename(next, link);
              });
            }, inPath, { excludedPatterns: ['build/'] });
          });

        it('judges a symlink named like a pattern for directories by where it leads, also once removed',
          async function() {
            const target = path.join(inPath, 'store');
            const link = path.join(inPath, 'node_modules');
            await mkdirp(target);
            await withWatchers(inPath, [], async ({ excluded }) => {
              await excluded(link, nsfw.actions.CREATED, () => fsp.symlink(target, link));
              await excluded(link, nsfw.actions.DELETED, () => remove(link));
            }, inPath, { excludedPatterns: ['node_modules/'] });
          });
      }

      it('reports the removal of a file named like a pattern for directories', async function() {
        const file = path.join(inPath, 'build');
        await withWatchers(inPath, [], async ({ actual }) => {
          await actual.observe(file, nsfw.actions.CREATED, () => fsp.writeFile(file, 'A file'));
          await actual.observe(file, nsfw.actions.DELETED, () => remove(file));
        }, inPath, { excludedPatterns: ['build/'] });
      });

      // macOS coalesces the events of one path, so whether it tells a replacement apart varies.
      (process.platform === 'darwin' ? it.skip : it)(
        'reports the removal of a file named like a pattern for directories that a directory replaced',
        async function() {
          const file = path.join(inPath, 'build');
          await withWatchers(inPath, [], async ({ actual, excluded }) => {
            await actual.observe(file, nsfw.actions.CREATED, () => fsp.writeFile(file, 'A file'));
            await actual.observe(file, nsfw.actions.DELETED, async () => {
              await remove(file);
              await fsp.mkdir(file);
            });
            await excluded(path.join(file, 'output.file'), undefined,
              () => fsp.writeFile(path.join(file, 'output.file'), 'Output'));
          }, inPath, { excludedPatterns: ['build/'] });
        });

      it('matches patterns with many stars in bounded time', async function() {
        const name = 'a'.repeat(80);
        const nested = path.join(inPath, ...Array(20).fill('a'));
        await mkdirp(nested);
        const patterns = ['*a*a*a*a*a*a*a*a*a*a*a*a*c', '**/a/'.repeat(24) + 'b'];
        await withWatchers(inPath, [], async ({ actual, excluded }) => {
          await exerciseExcludedFile(path.join(inPath, `${name}c`), excluded);
          // No match, which a matcher without wildmatch's early aborts, or with several "**", could take exponential
          // time to find.
          await expectReported(actual, inPath, name);
          await expectReported(actual, nested, 'c');
        }, inPath, { excludedPatterns: patterns });
      });

      it('applies the ignore files above the watched path in the repository', async function() {
        await makeRepository('*.tmp\n');
        const watched = path.join(inPath, 'folder4');
        await withWatchers(watched, [], async ({ actual, excluded }) => {
          await exerciseExcludedFile(path.join(watched, 'below.tmp'), excluded);
          await expectReported(actual, watched);
        }, watched, { respectGitignore: true });
      });

      if (isLinux) {
        it('never watches what an ignore file ignores while it is rewritten', async function() {
          await makeRepository('ignored/\n');
          await mkdirp(path.join(inPath, 'ignored', 'a', 'b', 'c'));
          const gitignore = path.join(inPath, '.gitignore');
          const before = countInotifyWatches();
          const watch = await nsfw(inPath, () => {}, { debounceMS, respectGitignore: true });
          try {
            await watch.start();
            // inPath, .git, folder4.
            await watchCountReaches(before + 3);
            const most = await peakWatchesDuring(before, async () => {
              // Truncated, then written 300 ms later; then removed and created again 300 ms later.
              await fsp.writeFile(gitignore, '');
              await sleep(300);
              await fsp.writeFile(gitignore, 'ignored/\n');
              await sleep(300);
              await remove(gitignore);
              await sleep(300);
              await fsp.writeFile(gitignore, 'ignored/\n');
              // Past the longest delay of an inclusion, 5 s after the first change.
              await sleep(5500);
            });
            assert.strictEqual(most, 3);
            assert.strictEqual(countInotifyWatches(), before + 3);
          } finally {
            await watch.stop();
          }
        });

        it('watches what an ignore file stops ignoring in a directory renamed meanwhile, once it settles',
          async function() {
            await makeRepository('');
            await mkdirp(path.join(inPath, 'a', 'out'));
            await fsp.writeFile(path.join(inPath, 'a', '.gitignore'), 'out/\n');
            const before = countInotifyWatches();
            const recorder = recordEvents();
            const watch = await nsfw(inPath, recorder.callback, { debounceMS, respectGitignore: true });
            try {
              await watch.start();
              // inPath, .git, folder4, a.
              await watchCountReaches(before + 4);
              const most = await peakWatchesDuring(before, async () => {
                await fsp.writeFile(path.join(inPath, 'a', '.gitignore'), '');
                await fsp.rename(path.join(inPath, 'a'), path.join(inPath, 'b'));
                // The rules applied before still count until the change settles, wherever the directory went.
                await sleep(600);
              });
              assert.strictEqual(most, 4);
              // And b/out once it settled.
              await watchCountReaches(before + 5);
              await eventuallyReported(recorder, path.join(inPath, 'b', 'out'));
            } finally {
              await watch.stop();
            }
          });

        it('reports what is written in a directory recreated without the rule that ignored it', async function() {
          await makeRepository('');
          await mkdirp(path.join(inPath, 'pkg', 'generated'));
          await fsp.writeFile(path.join(inPath, 'pkg', '.gitignore'), 'generated/\n');
          const recorder = recordEvents();
          const watch = await nsfw(inPath, recorder.callback, { debounceMS, respectGitignore: true });
          try {
            await watch.start();
            const barrier = markerBarrier(inPath, recorder);
            await barrier();
            await remove(path.join(inPath, 'pkg'));
            await mkdirp(path.join(inPath, 'pkg', 'generated'));
            await fsp.writeFile(path.join(inPath, 'pkg', '.gitignore'), '');
            await barrier();
            // Nothing the removed directory's rules decided is left: the new one is judged by its own.
            const written = path.join(inPath, 'pkg', 'generated', 'output.file');
            await recorder.observe(written, nsfw.actions.CREATED, () => fsp.writeFile(written, 'Output'));
          } finally {
            await watch.stop();
          }
        });

        it('does not watch a directory created while an ignore file is rewritten', async function() {
          await makeRepository('cache/\n');
          const gitignore = path.join(inPath, '.gitignore');
          const before = countInotifyWatches();
          const watch = await nsfw(inPath, () => {}, { debounceMS, respectGitignore: true });
          try {
            await watch.start();
            // inPath, .git, folder4.
            await watchCountReaches(before + 3);
            const most = await peakWatchesDuring(before, async () => {
              await fsp.writeFile(gitignore, '');
              await mkdirp(path.join(inPath, 'cache', 'x', 'y'));
              await sleep(300);
              await fsp.writeFile(gitignore, 'cache/\n');
              // Past the longest delay of an inclusion, 5 s after the first change.
              await sleep(5500);
            });
            assert.strictEqual(most, 3);
          } finally {
            await watch.stop();
          }
        });

        it('does not watch what is created in a directory an ignore file just started ignoring', async function() {
          await makeRepository('');
          await mkdirp(path.join(inPath, 'build'));
          const before = countInotifyWatches();
          const watch = await nsfw(inPath, () => {}, { debounceMS, respectGitignore: true });
          try {
            await watch.start();
            // inPath, .git, folder4, build.
            await watchCountReaches(before + 4);
            const most = await peakWatchesDuring(before, async () => {
              await fsp.writeFile(path.join(inPath, '.gitignore'), 'build/\n');
              await mkdirp(path.join(inPath, 'build', 'a', 'b', 'c'));
              await watchCountReaches(before + 3);
            });
            assert.strictEqual(most, 4);
          } finally {
            await watch.stop();
          }
        });

        it('does not watch an ignored directory through a symlink to it', async function() {
          await makeRepository('build/\n');
          await mkdirp(path.join(inPath, 'build', 'a', 'b'));
          await fsp.symlink(path.join(inPath, 'build'), path.join(inPath, 'current'));
          const before = countInotifyWatches();
          const watch = await nsfw(inPath, () => {}, { debounceMS, respectGitignore: true });
          try {
            await watch.start();
            // inPath, .git, folder4.
            await watchCountReaches(before + 3);
          } finally {
            await watch.stop();
          }
        });

        it('does not watch the directories it ignores', async function() {
          await makeRepository('ignored/\n');
          await mkdirp(path.join(inPath, 'ignored', 'a', 'b'));
          await mkdirp(path.join(inPath, 'patterned', 'a', 'b'));
          const before = countInotifyWatches();
          const recorder = recordEvents();
          const reported = recorder.events;
          const watch = await nsfw(inPath, recorder.callback,
            { debounceMS, respectGitignore: true, excludedPatterns: ['patterned'] });
          const barrier = markerBarrier(inPath, recorder);
          try {
            await watch.start();
            // inPath, .git, folder4.
            await watchCountReaches(before + 3);
            await fsp.writeFile(path.join(inPath, '.gitignore'), '');
            await watchCountReaches(before + 6);
            await barrier();
            // Included again silently: what the directories already held isn't reported.
            assert.deepStrictEqual(reported.filter(event => event.directory.includes('ignored')), []);
            await fsp.writeFile(path.join(inPath, '.gitignore'), 'ignored/\n');
            await watchCountReaches(before + 3);
            await fsp.mkdir(path.join(inPath, 'ignored-later'));
            await watchCountReaches(before + 4);
            await fsp.writeFile(path.join(inPath, '.gitignore'), 'ignored*/\n');
            await watchCountReaches(before + 3);
            await fsp.mkdir(path.join(inPath, 'patterned', 'a', 'c'));
            await watch.updateExcludedPatterns([]);
            // patterned, a, b, c.
            await watchCountReaches(before + 7);
            await watch.updateExcludedPatterns(['a']);
            await watchCountReaches(before + 4);
          } finally {
            await watch.stop();
          }
        });
      }
    });

    it('keeps excluded paths when watching a single file', async function () {
      const filePath = path.join(workDir, 'test0', 'testing0.file');

      const watch = await nsfw(filePath, () => {}, { excludedPaths: [filePath] });
      assert.deepEqual(await watch.getExcludedPaths(), canonicalPaths([filePath]));

      const returned = await watch.getExcludedPaths();
      returned.push(filePath);
      assert.deepEqual(await watch.getExcludedPaths(), canonicalPaths([filePath]));

      const results = await Promise.all([
        watch.updateExcludedPaths([]),
        watch.updateExcludedPaths([filePath + path.sep])
      ]);
      assert.deepEqual(results, [[], canonicalPaths([filePath])]);
      assert.deepEqual(await watch.getExcludedPaths(), canonicalPaths([filePath]));

      await assert.rejects(() => watch.updateExcludedPaths(['testing0.file']), /^Error: Excluded path must be/);
      assert.deepEqual(await watch.getExcludedPaths(), canonicalPaths([filePath]));
      assert.deepEqual(await watch.updateExcludedPaths([path.join(workDir, 'test1'), filePath]),
        canonicalPaths([filePath]));
      assert.deepEqual(await watch.updateExcludedPaths([path.join(workDir, 'test1')]), canonicalPaths([]));
      assert.deepEqual(await watch.getExcludedPaths(), canonicalPaths([]));
    });
  });

  describe('Garbage collection', function() {
    it('can garbage collect all instances', { timeout: 60000 }, async function () {
      let threw = false;
      try {
        // Try to get an error coming from the C++ constructor by passing a bad callback
        await nsfw(workDir, () => {}, { errorCallback: 'not a callback' });
      } catch (e) {
        threw = true;
      } if (!threw) {
        assert.fail('nsfw must throw when provided a bad callback');
      }
      while (nsfw.getAllocatedInstanceCount() > 0) {
        global.gc();
        await sleep(0);
      }
    });
  });

  describe('Process lifecycle', function() {
    const childScript = path.join(__dirname, 'lifecycle-child.js');

    function runChild(mode, watchDir, nodeArgs = [], timeoutMS = 20000) {
      return new Promise((resolve, reject) => {
        const child = cp.spawn(process.execPath, [...nodeArgs, childScript, mode, watchDir], { stdio: 'inherit' });
        const timer = setTimeout(() => {
          child.kill();
          reject(new Error(`child process (${mode}) did not exit on its own`));
        }, timeoutMS);
        child.on('exit', (code, signal) => {
          clearTimeout(timer);
          resolve({ code, signal });
        });
      });
    }

    it('lets the process exit after stop(), also after a restart', async function() {
      assert.deepEqual(await runChild('stop', path.join(workDir, 'test0')), { code: 0, signal: null });
    });

    it('stops once when stop() is called concurrently, and can start again', async function() {
      assert.deepEqual(await runChild('concurrent-stop', path.join(workDir, 'test3')), { code: 0, signal: null });
    });

    it('lets the process exit when the watcher errors and stop() is never called', async function() {
      assert.deepEqual(await runChild('error', path.join(workDir, 'test1')), { code: 0, signal: null });
    });

    it('tears down a worker whose watcher errored without stop()', async function() {
      assert.deepEqual(await runChild('worker-error', path.join(workDir, 'test2')), { code: 0, signal: null });
    });

    it('tears down workers terminated while many watchers are busy, and frees their queued events', async function() {
      // 15 workers that each start 40 watchers: give slow CI runners more time.
      assert.deepEqual(
        await runChild('worker-terminate-busy', path.join(workDir, 'test4'), ['--expose-gc'], 60000),
        { code: 0, signal: null }
      );
    });

    it('releases the callbacks of watchers that are collected', async function() {
      assert.deepEqual(
        await runChild('collect-callbacks', path.join(workDir, 'test5'), ['--expose-gc']),
        { code: 0, signal: null }
      );
    });

    it('can update excluded paths before start()', async function() {
      assert.deepEqual(await runChild('update-before-start', path.join(workDir, 'test6')), { code: 0, signal: null });
    });

    it('updates the excluded paths of many busy watchers without crashing', async function() {
      // 40 watchers whose excluded paths change while events flow: give slow CI runners more time.
      assert.deepEqual(
        await runChild('update-excluded-under-load', path.join(workDir, 'test7'), [], 60000),
        { code: 0, signal: null }
      );
    });
  });

  // Worker tests
  let Worker;
  try {
    Worker = require('worker_threads').Worker;
  } catch (e) { /* do nothing */ }

  if (Worker) {
    describe('Worker tests', function() {
      const local = path.join.bind(path, __dirname);

      beforeEach(async function() {
        try {
          await remove(workDir);
        } catch (e) {/* we don't care about this failure */}

        await fsp.mkdir(workDir);
      });

      afterEach(function() {
        return remove(workDir);
      });

      it('can kill worker thread while watcher is working', (t, done) => {
        const workerPath = local('./worker.js');
        const inPath = path.resolve(workDir);
        const worker = new Worker(workerPath, {
          workerData: {
            workDir: inPath,
            test: 1
          }
        });
        worker.on('message', (message) => {
          switch (message) {
            case 'init':
              break;
            case 'success':
              setTimeout(() => { worker.terminate(); }, 500);
              break;
            case 'failure':
              assert.fail();
              break;
          }
        });
        worker.on('error', () => assert.fail());
        worker.on('exit', (code) => {
          if (code === 1) {
            if (nsfw.getAllocatedInstanceCount() > 0) {
              assert.fail();
            } else {
              done();
            }
          } else {
            assert.fail();
          }
        });
      });

      it('can kill worker thread while 2 watcher is working and 1 watcher is stopped and freed', (t, done) => {
        const workerPath = local('./worker.js');
        const inPath = path.resolve(workDir);
        const worker = new Worker(workerPath, {
          workerData: {
            workDir: inPath,
            test: 2
          }
        });
        worker.on('message', (message) => {
          switch (message) {
            case 'init':
              break;
            case 'success':
              setTimeout(() => {
                worker.terminate();
              }, 500);
              break;
            case 'failure':
              assert.fail();
              break;
          }
        });
        worker.on('error', () => assert.fail());
        worker.on('exit', (code) => {
          if (code === 1) {
            done();
          } else {
            assert.fail();
          }
        });
      });
    });
  }
});
