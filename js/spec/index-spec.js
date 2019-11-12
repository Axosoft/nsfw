const assert = require('assert');
const exec = require('executive');
const fse = require('fs-extra');
const path = require('path');
const { promisify } = require('util');

const nsfw = require('../src/');

const DEBOUNCE = 1000;
const TIMEOUT_PER_STEP = 3000;

const sleep = promisify(setTimeout);

describe('Node Sentinel File Watcher', function() {
  this.timeout(120000);

  const workDir = path.resolve('./mockfs');
  const isWin = process.platform === 'win32';
  const isLinux = process.platform === 'linux';
  const isOsx = process.platform === 'darwin';

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

    it('can listen for a rename event', function(done) {
      const waitTimeout = () => new Promise(resolve => {
        setTimeout(resolve, TIMEOUT_PER_STEP);
      });
      const srcFile = 'testing.file';
      const destFile = 'new-testing.file';
      const inPath = path.resolve(workDir, 'test4');
      let eventListening = false;
      let eventFound = false;
      let extraEventFound = false;

      function findEvent(element) {
        if (!eventListening) {
          return;
        }
        if (
          element.action === nsfw.actions.RENAMED &&
          element.directory === path.resolve(inPath) &&
          element.oldFile === srcFile &&
          element.newDirectory === path.resolve(inPath) &&
          element.newFile === destFile
        ) {
          eventFound = true;
        } else {
          if (element.directory === path.resolve(inPath)) {
            extraEventFound = true;
          }
        }
      }

      let watch;

      return nsfw(
        workDir,
        events => events.forEach(findEvent),
        { debounceMS: DEBOUNCE }
      )
        .then(_w => {
          watch = _w;
          return watch.start();
        })
        .then(waitTimeout)
        .then(() => {
          fse.ensureFileSync(path.join(inPath, srcFile));
        })
        .then(waitTimeout)
        .then(() => {
          eventListening = true;
          return fse.move(path.join(inPath, srcFile), path.join(inPath, destFile));
        })
        .then(waitTimeout)
        .then(() => {
          eventListening = false;
        })
        .then(() => {
          expect(eventFound).toBe(true);
          expect(extraEventFound).toBe(false);
          return watch.stop();
        })
        .then(done, () => {
          watch.stop().then((err) => done.fail(err));
        });
    });

    it('can listen for a move event', function(done) {
      const waitTimeout = () => new Promise(resolve => {
        setTimeout(resolve, TIMEOUT_PER_STEP);
      });
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

      let watch;

      return nsfw(
        workDir,
        events => events.forEach(findEvent),
        { debounceMS: DEBOUNCE }
      )
        .then(_w => {
          watch = _w;
          return watch.start();
        })
        .then(waitTimeout)
        .then(() => {
          fse.ensureFileSync(path.join(srcInPath, file));
          fse.ensureDirSync(path.join(destInPath));
        })
        .then(waitTimeout)
        .then(() => {
          eventListening = true;
          return fse.move(path.join(srcInPath, file), path.join(destInPath, file));
        })
        .then(waitTimeout)
        .then(() => {
          eventListening = false;
        })
        .then(() => {
          if (isWin) {
            expect(deleteEventFound && createEventFound).toBe(true);
            expect(renameEventFound).toBe(false);
          }
          if (isLinux || isOsx) {
            expect(renameEventFound).toBe(true);
            expect(deleteEventFound || createEventFound).toBe(false);
          }
          expect(extraEventFound).toBe(false);
          return watch.stop();
        })
        .then(done, () => {
          watch.stop().then((err) => done.fail(err));
        });
    });

    it('can run multiple watchers at once', async function() {
      const dirA = path.resolve(workDir, 'test0');
      const fileA = 'testing1.file';
      const dirB = path.resolve(workDir, 'test1');
      const fileB = 'testing0.file';
      let events = 0;

      function findEvent(element) {
        if (
          element.action === nsfw.actions.CREATED
        ) {
          if (element.directory === dirA && element.file === fileA) {
            events++;
          } else if (element.directory === dirB && element.file === fileB) {
            events++;
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

        assert.equal(events, 2);
      } finally {
        await Promise.all([watchA.stop(), watchB.stop()]);
        watchA = null;
        watchB = null;
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
    it('can listen for the creation of a deeply nested file', async function() {
      const paths = ['d', 'e', 'e', 'p', 'f', 'o', 'l', 'd', 'e', 'r'];
      const file = 'a_file.txt';
      let foundFileCreateEvent = false;

      function findEvent(element) {
        if (
          element.action === nsfw.actions.CREATED &&
          element.directory === path.join(workDir, ...paths) &&
          element.file === file
        ) {
          foundFileCreateEvent = true;
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
        let directory = workDir;
        for (const dir of paths) {
          directory = path.join(directory, dir);
          await fse.mkdir(directory);
          await sleep(60);
        }
        const fd = await fse.open(path.join(directory, file), 'w');
        await fse.close(fd);
        await sleep(TIMEOUT_PER_STEP);

        assert.ok(foundFileCreateEvent);
      } finally {
        await watch.stop();
        watch = null;
      }
    });

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

  describe('Errors', function() {
    it('can gracefully recover when the watch folder is deleted', async function() {
      const inPath = path.join(workDir, 'test4');
      let erroredOut = false;
      let watch = await nsfw(
        inPath,
        () => {},
        { debounceMS: DEBOUNCE, errorCallback() { erroredOut = true; } }
      );

      try {
        await watch.start();
        await sleep(TIMEOUT_PER_STEP);
        await fse.remove(inPath);
        await sleep(TIMEOUT_PER_STEP);

        assert.ok(erroredOut);
      } finally {
        await watch.stop();
        watch = null;
      }
    });
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
            errorRestart = errorRestart.then(async () => {
              await watch.stop();
              await watch.start();
            });
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
              errorRestart = errorRestart.then(async () => {
                await watch.stop();
                await watch.start();
              });
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
      const file = 'unicoded_right_in_the.talker';
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

  describe('Garbage collection', function() {
    it('can garbage collect all instances', async function () {
      this.timeout(60000);
      while (nsfw.getAllocatedInstanceCount() > 0) {
        global.gc();
        await sleep(0);
      }
    });
  });
});
