const assert = require('assert');
const cp = require('child_process');
const exec = require('executive');
const fse = require('fs-extra');
const path = require('path');

const { DEBOUNCE, TIMEOUT_PER_STEP, WORKDIR: workDir, sleep } = require('./common');

const nsfw = require('../src');

describe('Node Sentinel File Watcher', function() {
  this.timeout(120000);

  assert.ok(typeof nsfw._native.NSFW_TEST_SLOW === 'undefined', 'NSFW should NOT be built in slow mode');

  beforeEach(async function() {
    async function makeDir(identifier) {
      await fse.mkdir(path.join(workDir, 'test' + identifier));
      await fse.mkdir(path.join(workDir, 'test' + identifier, 'folder' + identifier));
      const fd = await fse.open(path.join(workDir, 'test' + identifier, 'testing' + identifier +'.file'), 'w');
      await fse.write(fd, 'testing');
      await fse.close(fd);
    }

    try {
      await fse.remove(workDir);
    } catch (e) {/* we don't care about this failure */}

    await fse.mkdir(workDir);
    const promises = [];
    for (let i = 0; i < 10; ++i) {
      promises.push(makeDir(i));
    }

    await Promise.all(promises);
  });

  afterEach(function() {
    return fse.remove(workDir);
  });

  describe('Basic', function() {
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
        await sleep(TIMEOUT_PER_STEP);
        await fse.writeFile(filePath, 'Bean bag video games at noon.');
        await sleep(TIMEOUT_PER_STEP);
        await fse.remove(filePath);
        await sleep(TIMEOUT_PER_STEP);
        await fse.writeFile(filePath, 'His watch has ended.');
        await sleep(TIMEOUT_PER_STEP);

        assert.ok(changeEvents > 0);
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
        await sleep(TIMEOUT_PER_STEP);
        await fse.writeFile(path.join(inPath, file), 'Peanuts, on occasion, rain from the skies.');
        await sleep(TIMEOUT_PER_STEP);

        assert.ok(eventFound);
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
        await sleep(TIMEOUT_PER_STEP);
        await fse.remove(path.join(inPath, file));
        await sleep(TIMEOUT_PER_STEP);

        assert.ok(eventFound);
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
        await sleep(TIMEOUT_PER_STEP);
        await fse.writeFile(path.join(inPath, file), 'At times, sunflower seeds are all that is life.');
        await sleep(TIMEOUT_PER_STEP);

        assert(eventFound);
      } finally {
        await watch.stop();
        watch = null;
      }
    });

    if (process.platform !== 'darwin') {
      // this test is super flakey on CI right now
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

        let watch = await nsfw(
          workDir,
          events => events.forEach(findEvent),
          { debounceMS: DEBOUNCE }
        );

        try {
          await watch.start();
          await sleep(TIMEOUT_PER_STEP);
          await fse.ensureFile(path.join(inPath, srcFile));
          await sleep(TIMEOUT_PER_STEP);
          eventListening = true;
          await fse.move(path.join(inPath, srcFile), path.join(inPath, destFile));
          await sleep(TIMEOUT_PER_STEP);
          eventListening = false;

          switch (process.platform) {
            case 'darwin':
              assert.ok(deleteEventFound && createEventFound !== renameEventFound);
              break;

            default:
              assert.ok(renameEventFound);
              assert.ok(!deleteEventFound && !createEventFound);
              break;
          }

          assert.ok(!extraEventFound);
        } finally {
          await watch.stop();
          watch = null;
        }
      });
    }

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

      let watch = await nsfw(
        workDir,
        events => events.forEach(findEvent),
        { debounceMS: DEBOUNCE }
      );

      try {
        await watch.start();
        await sleep(TIMEOUT_PER_STEP);
        await fse.ensureFile(path.join(srcInPath, file));
        await fse.ensureDir(path.join(destInPath));
        await sleep(TIMEOUT_PER_STEP);
        eventListening = true;
        await fse.move(path.join(srcInPath, file), path.join(destInPath, file));
        await sleep(TIMEOUT_PER_STEP);
        eventListening = false;

        switch (process.platform) {
          case 'linux':
            assert.ok(renameEventFound);
            assert.ok(!deleteEventFound && !createEventFound);
            break;

          default:
            assert.ok(deleteEventFound && createEventFound);
            assert.ok(!renameEventFound);
            break;
        }

        assert.ok(!extraEventFound);
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
        await sleep(TIMEOUT_PER_STEP);
        await Promise.all([
          fse.writeFile(path.join(dirA, fileA), 'At times, sunflower seeds are all that is life.'),
          fse.writeFile(path.join(dirB, fileB), 'At times, sunflower seeds are all that is life.')
        ]);
        await sleep(TIMEOUT_PER_STEP);

        assert.ok(createdA, 'watcher A did not report the created file');
        assert.ok(createdB, 'watcher B did not report the created file');
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
          fse.writeFile(path.join(dirs[i], file), 'Not a moment too soon.')
        )));
        await sleep(TIMEOUT_PER_STEP);

        assert.deepEqual(dirs.filter(dir => !seen.has(dir)), []);
      } finally {
        await Promise.all(watches.map(watch => watch.stop()));
        watches = null;
      }
    });

    it('will properly track the movement of watched directories across watched directories', async function() {
      const performRenameProcedure = async (number) => {
        await fse.mkdir(path.join(workDir, `test${number}`, 'sneaky-folder'));
        await fse.move(
          path.join(workDir, `test${number}`, `folder${number}`),
          path.join(workDir, `test${number + 1}`, 'bad-folder')
        );
        await fse.move(
          path.join(workDir, `test${number}`, 'sneaky-folder'),
          path.join(workDir, `test${number}`, 'bad-folder')
        );
        await fse.remove(path.join(workDir, `test${number}`));
        await fse.remove(path.join(workDir, `test${number + 1}`));
      };

      let watch = await nsfw(workDir, () => {}, { debounceMS: DEBOUNCE });

      try {
        await watch.start();
        await sleep(TIMEOUT_PER_STEP);
        await Promise.all([
          performRenameProcedure(0),
          performRenameProcedure(2),
          performRenameProcedure(4),
          performRenameProcedure(6),
          performRenameProcedure(8)
        ]);
        await sleep(TIMEOUT_PER_STEP);
      } finally {
        await watch.stop();
        watch = null;
      }
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
        await sleep(TIMEOUT_PER_STEP);
        await fse.remove(inPath);
        await sleep(TIMEOUT_PER_STEP);

        assert.ok(deletionCount > 2);
      } finally {
        await watch.stop();
        watch = null;
      }
    });

    it('does not loop endlessly when watching directories with recursive symlinks', async function () {
      await fse.mkdir(path.join(workDir, 'test'));
      await fse.symlink(path.join(workDir, 'test'), path.join(workDir, 'test', 'link'));

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
      const file = 'another_test.file';
      const inPath = path.resolve(workDir, 'test2', 'folder2');
      let eventFound = false;

      function findEvent(element) {
        if (
          (
            element.action === nsfw.actions.CREATED ||
            element.action === nsfw.actions.MODIFIED
          ) &&
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
        await watch.pause();
        await sleep(TIMEOUT_PER_STEP);
        await fse.writeFile(path.join(inPath, file), 'You will not see me.');
        await sleep(TIMEOUT_PER_STEP);

        assert.ok(!eventFound);

        await watch.resume();
        await sleep(TIMEOUT_PER_STEP);
        assert.ok(!eventFound);

        await fse.writeFile(path.join(inPath, file), 'But you will see me.');
        await sleep(TIMEOUT_PER_STEP);

        assert.ok(eventFound);
      } finally {
        await watch.stop();
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
        await sleep(TIMEOUT_PER_STEP);
        await fse.remove(inPath);
        await sleep(TIMEOUT_PER_STEP);

        assert.ok(erroredOut);
        await assert.rejects(watch.stop(), /not running/);
        await fse.mkdir(inPath);
        await watch.start();
        await fse.writeFile(path.join(inPath, 'after-recovery'), 'watching again');
        await received;
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
        await sleep(TIMEOUT_PER_STEP);
        await fse.rename(inPath, renamed);
        await sleep(TIMEOUT_PER_STEP);

        assert.ok(errorOk);
      } finally {
        try {
          await watch.stop();
        } catch (err) { /* do nothing */ }
        watch = null;
      }
    });

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
          await sleep(100);
          await watch.start();
          await sleep(TIMEOUT_PER_STEP);
          await fse.remove(inPath);
          await sleep(TIMEOUT_PER_STEP);

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
          await fse.mkdir(inPath);
          await sleep(TIMEOUT_PER_STEP);
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
          await sleep(TIMEOUT_PER_STEP);
          await fse.rename(parentPath, renamed);
          await sleep(TIMEOUT_PER_STEP);

          assert.ok(errorOk);
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
          await fse.mkdir(inPath);
          await sleep(TIMEOUT_PER_STEP);
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
          await sleep(TIMEOUT_PER_STEP);
          await fse.remove(parentPath);
          await sleep(TIMEOUT_PER_STEP);

          assert.ok(errorOk);
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
      return exec('git clone https://github.com/implausible/nsfw-stress-test');
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
        await fse.remove(path.join(stressRepoPath, 'folder'));
        await errorRestart;
        assert.ok(count > 0);

        await watch.stop();
        await fse.remove(stressRepoPath);
        await fse.mkdir(stressRepoPath);

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
        await sleep(TIMEOUT_PER_STEP);
        await exec(
          `git clone https://github.com/implausible/nsfw-stress-test ${path.join('nsfw-stress-test', 'test')}`
        );
        await fse.stat(path.join(stressRepoPath, 'test'));
        await fse.remove(path.join(stressRepoPath, 'test'));
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
      return fse.remove(stressRepoPath);
    });
  });

  describe('Unicode support', function() {
    const watchPath = path.join(workDir, 'は');
    beforeEach(function() {
      return fse.mkdir(watchPath);
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
        await sleep(TIMEOUT_PER_STEP);
        await fse.writeFile(path.join(watchPath, file), 'Unicode though.');
        await sleep(TIMEOUT_PER_STEP);

        assert.ok(eventFound);
      } finally {
        await watch.stop();
        watch = null;
      }
    });
  });

  describe('Excluded paths', function() {
    const debounceMS = 100;
    const eventTimeoutMS = 10000;
    const absenceWindowMS = 500;

    function recordEvents() {
      const events = [];
      const listeners = new Set();

      return {
        events,
        callback(batch) {
          events.push(...batch);
          listeners.forEach(listener => listener());
        },
        async observe(filePath, action, operation) {
          const offset = events.length;
          let listener, timer;
          const received = new Promise((resolve, reject) => {
            listener = () => {
              if (events.slice(offset).some(event =>
                matches(event, filePath) && (action === undefined || event.action === action))) {
                resolve();
              }
            };
            listeners.add(listener);
            timer = setTimeout(() => reject(new Error(
              `No action ${action} observed for ${filePath}: ${JSON.stringify(events.slice(offset))}`
            )), eventTimeoutMS);
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

    function matches(event, filePath) {
      return event.directory === path.dirname(filePath) && event.file === path.basename(filePath);
    }

    // reportedPath is where the watcher reports inPath's events, when that differs (macOS resolves symlinks).
    async function withWatchers(inPath, excludedPaths, test, reportedPath = inPath) {
      const actual = recordEvents();
      const control = recordEvents();
      const watch = await nsfw(inPath, actual.callback, { debounceMS, excludedPaths });
      const witness = await nsfw(inPath, control.callback, { debounceMS });
      let marker = 0;

      async function barrier() {
        const markerPath = path.join(reportedPath, `barrier-${marker++}.file`);
        await actual.observe(markerPath, nsfw.actions.CREATED, () => fse.writeFile(markerPath, 'Barrier'));
      }

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
        await test({ watch, actual, barrier, excluded });
      } finally {
        await Promise.all([watch.stop(), witness.stop()]);
      }
    }

    async function appendFile(filePath, contents) {
      await fse.appendFile(filePath, contents);
      // FSEvents demangles combined flags using birth/mtime seconds, so keep those seconds distinct.
      await fse.utimes(filePath, new Date(), new Date(Date.now() + 60000));
    }

    async function exerciseExcludedFile(filePath, excluded) {
      await excluded(filePath, nsfw.actions.CREATED, () => fse.writeFile(filePath, 'Excluded.'));
      await excluded(filePath, nsfw.actions.DELETED, () => fse.remove(filePath));
      await excluded(filePath, nsfw.actions.CREATED, () => fse.writeFile(filePath, 'Excluded again.'));
      await excluded(filePath, undefined, () => fse.appendFile(filePath, 'Append'));
    }

    it('can ignore events from excluded paths', async function() {
      const inPath = path.join(workDir, 'test4');
      const excludedPath = path.join(inPath, 'excluded');
      await fse.mkdir(excludedPath);

      await withWatchers(inPath, [excludedPath], async ({ actual, excluded }) => {
        await exerciseExcludedFile(path.join(excludedPath, 'excluded.file'), excluded);
        const includedFile = path.join(inPath, 'testing4.file');
        await actual.observe(includedFile, nsfw.actions.DELETED, () => fse.remove(includedFile));
      });
    });

    it('can remove an excluded path', async function() {
      const excludedPath = path.join(workDir, 'T1', 'T2');
      await fse.ensureDir(path.join(excludedPath, 'T3'));
      const filePath = path.join(excludedPath, 'newfile.file');

      await withWatchers(workDir, [excludedPath], async ({ watch, actual, barrier, excluded }) => {
        await excluded(filePath, nsfw.actions.CREATED, () => fse.writeFile(filePath, 'New file.'));
        await watch.updateExcludedPaths([]);
        await actual.observe(filePath, nsfw.actions.MODIFIED, () => appendFile(filePath, 'Now included.'));
        await barrier();
        assert.ok(!actual.events.some(event => matches(event, filePath) && event.action === nsfw.actions.CREATED));
        assert.ok(!actual.events.some(event => matches(event, filePath) && event.action === nsfw.actions.DELETED));
      });
    });

    it('can add an excluded path', async function() {
      const excludedPath = path.join(workDir, 'T1', 'T2');
      await fse.ensureDir(path.join(excludedPath, 'T3'));
      const filePath = path.join(excludedPath, 'newfile.file');

      await withWatchers(workDir, [], async ({ watch, actual, barrier, excluded }) => {
        await actual.observe(filePath, nsfw.actions.CREATED, () => fse.writeFile(filePath, 'New file.'));
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
      await Promise.all([fse.mkdir(excludedPath), fse.mkdir(newExcludedPath)]);

      await withWatchers(inPath, [excludedPath], async ({ watch, actual, excluded }) => {
        assert.deepStrictEqual(await watch.getExcludedPaths(), [excludedPath]);
        await watch.updateExcludedPaths([excludedPath, newExcludedPath]);
        assert.deepStrictEqual(await watch.getExcludedPaths(), [excludedPath, newExcludedPath]);
        await exerciseExcludedFile(path.join(excludedPath, 'excluded.file'), excluded);
        await exerciseExcludedFile(path.join(newExcludedPath, 'excluded.file'), excluded);
        const includedFile = path.join(inPath, 'testing4.file');
        await actual.observe(includedFile, nsfw.actions.DELETED, () => fse.remove(includedFile));
      });
    });

    it('does not exclude a sibling whose name starts with the excluded name', async function() {
      const inPath = path.join(workDir, 'test4');
      const excludedPath = path.join(inPath, 'foo');
      const siblingPath = path.join(inPath, 'foobar');
      await Promise.all([fse.mkdir(excludedPath), fse.mkdir(siblingPath)]);

      await withWatchers(inPath, [excludedPath], async ({ actual, excluded }) => {
        await exerciseExcludedFile(path.join(excludedPath, 'excluded.file'), excluded);
        const siblingFile = path.join(siblingPath, 'sibling.file');
        await actual.observe(siblingFile, nsfw.actions.CREATED, () => fse.writeFile(siblingFile, 'Sibling.'));
      });
    });

    it('strips trailing separators from excluded paths', async function() {
      const inPath = path.join(workDir, 'test4');
      const excludedPath = path.join(inPath, 'excluded');
      await fse.mkdir(excludedPath);

      await withWatchers(inPath, [excludedPath + path.sep + path.sep], async ({ watch, actual, excluded }) => {
        assert.deepStrictEqual(await watch.getExcludedPaths(), [excludedPath]);
        await exerciseExcludedFile(path.join(excludedPath, 'excluded.file'), excluded);
        const includedFile = path.join(inPath, 'included.file');
        await actual.observe(includedFile, nsfw.actions.CREATED, () => fse.writeFile(includedFile, 'Included.'));

        assert.deepStrictEqual(await watch.updateExcludedPaths([excludedPath + '/' + path.sep]), [excludedPath]);
        assert.deepStrictEqual(await watch.getExcludedPaths(), [excludedPath]);
      });
    });

    it('excludes paths under a watched path that goes through a symlink', async function() {
      if (process.platform === 'win32') {
        // Creating a symlink needs extra privileges on Windows.
        this.skip();
      }
      const linkPath = path.join(workDir, 'test4link');
      await fse.symlink(path.join(workDir, 'test4'), linkPath, 'dir');
      const excludedPath = path.join(linkPath, 'excluded');
      const otherPath = path.join(linkPath, 'other');
      await Promise.all([fse.mkdir(excludedPath), fse.mkdir(otherPath)]);
      // macOS reports the events under the real path, here without the link; Linux under the watched path.
      const reportedPath = process.platform === 'darwin' ? await fse.realpath(linkPath) : linkPath;
      const reported = (directory, file) => path.join(reportedPath, path.relative(linkPath, directory), file);

      await withWatchers(linkPath, [excludedPath], async ({ watch, actual, excluded }) => {
        await exerciseExcludedFile(reported(excludedPath, 'excluded.file'), excluded);
        const includedFile = reported(linkPath, 'included.file');
        await actual.observe(includedFile, nsfw.actions.CREATED, () => fse.writeFile(includedFile, 'Included.'));
        assert.deepStrictEqual(await watch.getExcludedPaths(), [excludedPath]);

        // Also when they change while watching.
        assert.deepStrictEqual(await watch.updateExcludedPaths([otherPath]), [otherPath]);
        await exerciseExcludedFile(reported(otherPath, 'other.file'), excluded);
        const reincludedFile = reported(excludedPath, 'reincluded.file');
        await actual.observe(reincludedFile, nsfw.actions.CREATED, () => fse.writeFile(reincludedFile, 'Included.'));
      }, reportedPath);
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
      assert.deepEqual(await watch.getExcludedPaths(), [validPath]);
      for (const relativePath of relativePaths) {
        await assert.rejects(
          () => watch.updateExcludedPaths([relativePath]),
          /^Error: Excluded path must be an absolute path\.$/,
          `accepted ${relativePath} on update`
        );
      }
      await assert.rejects(() => watch.updateExcludedPaths(validPath), TypeError);
      await assert.rejects(() => watch.updateExcludedPaths([validPath, 1]), TypeError);
      assert.deepEqual(await watch.getExcludedPaths(), [validPath]);

      assert.deepEqual(await watch.updateExcludedPaths(outsidePaths), []);
      assert.deepEqual(await watch.getExcludedPaths(), []);

      const validPaths = [inPath, validPath, path.join(inPath, '..excluded')];
      if (process.platform === 'win32') {
        // The native watcher adds this prefix to every path, so either form counts as inside.
        validPaths.push(path.toNamespacedPath(validPath));
      }
      const mixedPaths = [outsidePaths[0], ...validPaths, outsidePaths[1]];
      assert.deepEqual(await watch.updateExcludedPaths(mixedPaths), validPaths);
      assert.deepEqual(await watch.getExcludedPaths(), validPaths);
      const constructed = await nsfw(inPath, () => {}, { excludedPaths: mixedPaths });
      assert.deepEqual(await constructed.getExcludedPaths(), validPaths);
    });

    it('keeps reporting events when the excluded paths are outside the watched directory', async function() {
      const inPath = path.join(workDir, 'proj');
      await fse.mkdir(inPath);
      const outsidePaths = [
        // A linked worktree's git directory, under its main repository next to the watched one.
        path.join(workDir, 'proj-main', '.git', 'worktrees', 'proj', 'lfs', 'tmp'),
        // An ancestor would match every event in the native watchers.
        workDir,
        // Shares the watched path as a prefix.
        `${inPath}-sibling`
      ];

      await withWatchers(inPath, outsidePaths, async ({ watch, actual }) => {
        assert.deepStrictEqual(await watch.getExcludedPaths(), []);
        const createdFile = path.join(inPath, 'created.file');
        await actual.observe(createdFile, nsfw.actions.CREATED, () => fse.writeFile(createdFile, 'Created.'));

        assert.deepStrictEqual(await watch.updateExcludedPaths(outsidePaths), []);
        const updatedFile = path.join(inPath, 'updated.file');
        await actual.observe(updatedFile, nsfw.actions.CREATED, () => fse.writeFile(updatedFile, 'Updated.'));
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
        assert.deepEqual(results, [first, second]);
        assert.deepEqual(await watch.getExcludedPaths(), second);
      } finally {
        await watch.stop();
      }
    });

    it('keeps excluded paths when watching a single file', async function () {
      const filePath = path.join(workDir, 'test0', 'testing0.file');

      const watch = await nsfw(filePath, () => {}, { excludedPaths: [filePath] });
      assert.deepEqual(await watch.getExcludedPaths(), [filePath]);

      const returned = await watch.getExcludedPaths();
      returned.push(filePath);
      assert.deepEqual(await watch.getExcludedPaths(), [filePath]);

      const results = await Promise.all([
        watch.updateExcludedPaths([]),
        watch.updateExcludedPaths([filePath + path.sep])
      ]);
      assert.deepEqual(results, [[], [filePath]]);
      assert.deepEqual(await watch.getExcludedPaths(), [filePath]);

      await assert.rejects(() => watch.updateExcludedPaths(['testing0.file']), /^Error: Excluded path must be/);
      assert.deepEqual(await watch.getExcludedPaths(), [filePath]);
      assert.deepEqual(await watch.updateExcludedPaths([path.join(workDir, 'test1'), filePath]), [filePath]);
      assert.deepEqual(await watch.updateExcludedPaths([path.join(workDir, 'test1')]), []);
      assert.deepEqual(await watch.getExcludedPaths(), []);
    });
  });

  describe('Garbage collection', function() {
    it('can garbage collect all instances', async function () {
      this.timeout(60000);
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

    it('tears down workers terminated while many watchers are busy', async function() {
      // 15 workers that each start 40 watchers: give slow CI runners more time.
      assert.deepEqual(
        await runChild('worker-terminate-busy', path.join(workDir, 'test4'), [], 60000),
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
          await fse.remove(workDir);
        } catch (e) {/* we don't care about this failure */}

        await fse.mkdir(workDir);
      });

      afterEach(function() {
        return fse.remove(workDir);
      });

      it('can kill worker thread while watcher is working', (done) => {
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

      it('can kill worker thread while 2 watcher is working and 1 watcher is stopped and freed', (done) => {
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
