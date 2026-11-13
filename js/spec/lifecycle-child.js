/* global FinalizationRegistry, SharedArrayBuffer, Atomics */
// Runs in a child process (and, for 'worker-error', in a worker thread inside it) so that a watcher that keeps the
// event loop alive, or aborts during teardown, cannot hang or crash the mocha process.
const { Worker, isMainThread, parentPort, workerData } = require('worker_threads');
const { createHook } = require('async_hooks');
const fs = require('fs');
const fse = require('fs-extra');
const path = require('path');

const nsfw = require('../src');

const [mode, watchDir] = isMainThread ? process.argv.slice(2) : workerData;

const sleep = ms => new Promise(resolve => setTimeout(resolve, ms));

async function stopAndRestart() {
  const watch = await nsfw(watchDir, () => {});
  await watch.start();
  await watch.stop();
  await watch.start();
  await watch.stop();
}

async function concurrentStops() {
  const watch = await nsfw(watchDir, () => {});
  await watch.start();
  const results = await Promise.allSettled([watch.stop(), watch.stop()]);
  if (results.filter(result => result.status === 'fulfilled').length !== 1) {
    throw new Error('exactly one concurrent stop() must succeed');
  }
  await watch.start();
  await watch.stop();
}

async function orderedJobs() {
  const watch = await nsfw(watchDir, () => {});
  for (let i = 0; i < 100; i++) {
    await Promise.all([watch.start(), watch.stop(), watch.start(), watch.pause(), watch.resume(), watch.stop()]);
  }
  const results = await Promise.allSettled([watch.stop(), watch.start(), watch.start(), watch.stop()]);
  const statuses = results.map(result => result.status);
  if (JSON.stringify(statuses) !== JSON.stringify(['rejected', 'fulfilled', 'rejected', 'fulfilled'])) {
    throw new Error(`unexpected lifecycle results: ${statuses}`);
  }
}

async function restartAfterError() {
  let onError;
  let onEvent;
  const watch = await nsfw(watchDir, events => onEvent && onEvent(events), {
    debounceMS: 1,
    errorCallback: error => onError(error)
  });
  await watch.start();
  for (let i = 0; i < 5; i++) {
    const restarted = new Promise((resolve, reject) => {
      onError = () => {
        fse.mkdirSync(watchDir);
        watch.start().then(resolve, reject);
      };
    });
    await fse.remove(watchDir);
    await restarted;
    const file = `after-error-${i}`;
    const received = new Promise(resolve => {
      onEvent = events => {
        if (events.some(event => event.file === file)) resolve();
      };
    });
    await fse.writeFile(path.join(watchDir, file), 'restarted');
    await received;
  }
  await watch.stop();
}

async function discardStoppedRunError() {
  let errors = 0;
  let stopped;
  let markerCreated;
  let resolveDrained;
  const drained = new Promise(resolve => { resolveDrained = resolve; });
  const marker = path.join(watchDir, 'stop-completed');
  const directory = path.join(watchDir, 'watched');
  await fse.mkdir(directory);
  const watch = await nsfw(directory, () => {}, { errorCallback: () => { errors++; } });
  const resources = [];
  let capturing = true;
  const hook = createHook({
    init(id, type) {
      if (capturing && type === 'nsfw') resources.push(id);
    },
    before(id) {
      if (id !== errorResource || stopped) return;
      stopped = watch.stop();
      // With one pool thread, mkdir runs after the native stop. Its on-disk marker lets us wait without
      // yielding to JS error delivery, which is already queued and about to enter the native callback.
      markerCreated = new Promise((resolve, reject) => {
        fs.mkdir(marker, error => error ? reject(error) : resolve());
      });
      const deadline = Date.now() + 5000;
      const wait = new Int32Array(new SharedArrayBuffer(4));
      while (!fs.existsSync(marker)) {
        if (Date.now() > deadline) throw new Error('native stop barrier did not complete');
        Atomics.wait(wait, 0, 0, 1);
      }
    },
    destroy(id) {
      if (id === errorResource) {
        hook.disable();
        resolveDrained();
      }
    }
  }).enable();
  // start() creates its job, event TSFN and error TSFN synchronously, in that order.
  const started = watch.start();
  capturing = false;
  if (resources.length !== 3) throw new Error(`unexpected start resources: ${resources.length}`);
  const errorResource = resources[2];
  await started;
  fse.removeSync(directory);
  await drained;
  await stopped;
  await markerCreated;
  if (!stopped) throw new Error('queued error was not intercepted');
  if (errors !== 0) throw new Error(`stopped run delivered ${errors} stale errors`);
}

async function collectErroredWatchers() {
  let collected = 0;
  const registry = new FinalizationRegistry(() => { collected++; });
  const baseline = nsfw.getAllocatedInstanceCount();
  const createWatcher = async i => {
    let resolveError;
    const errored = new Promise(resolve => { resolveError = resolve; });
    const directory = path.join(watchDir, String(i));
    await fse.mkdir(directory);
    const eventCallback = () => watch;
    const errorCallback = () => {
      resolveError();
      return watch;
    };
    const watch = await nsfw(directory, eventCallback, { errorCallback });
    registry.register(watch);
    registry.register(eventCallback);
    registry.register(errorCallback);
    await watch.start();
    await fse.remove(directory);
    await errored;
  };
  for (let i = 0; i < 10; i++) await createWatcher(i);
  for (let i = 0; i < 100 && (collected < 30 || nsfw.getAllocatedInstanceCount() !== baseline); i++) {
    global.gc();
    await sleep(20);
  }
  if (collected !== 30 || nsfw.getAllocatedInstanceCount() !== baseline) {
    throw new Error(`errored watchers retained: ${collected}/30 collected, ${nsfw.getAllocatedInstanceCount()} native`);
  }
}

async function terminateQueuedWorkers() {
  for (let i = 0; i < 10; i++) {
    const worker = new Worker(__filename, { workerData: ['queued-jobs', watchDir] });
    await new Promise((resolve, reject) => {
      worker.once('message', resolve);
      worker.once('error', reject);
    });
    await worker.terminate();
  }
}

async function queueJobs() {
  const watches = await Promise.all(Array.from({ length: 40 }, () => nsfw(watchDir, () => {})));
  for (const watch of watches) {
    for (let i = 0; i < 10; i++) {
      watch.start().catch(() => {});
      watch.stop().catch(() => {});
    }
  }
  parentPort.postMessage('queued');
}

async function errorWithoutStop() {
  let resolveError;
  const errored = new Promise(resolve => { resolveError = resolve; });
  const watch = await nsfw(watchDir, () => {}, { errorCallback: resolveError });
  await watch.start();
  await fse.remove(watchDir);
  await errored;
}

// Needs --expose-gc. Watchers that were never started, or were stopped, must not keep their callbacks alive, also
// when a callback captures its own watcher.
async function collectCallbacks() {
  let collected = 0;
  const registry = new FinalizationRegistry(() => { collected++; });
  const createWatcher = async (start, captureWatcher) => {
    let watch = null;
    const callback = captureWatcher ? () => watch : () => {};
    registry.register(callback);
    watch = await nsfw(watchDir, callback);
    if (start) {
      await watch.start();
      await watch.stop();
    }
  };
  for (let i = 0; i < 10; i++) {
    await createWatcher(i % 2 === 0, i % 4 < 2);
  }
  for (let i = 0; i < 20 && collected < 10; i++) {
    global.gc();
    await sleep(50);
  }
  if (collected !== 10) {
    throw new Error(`only ${collected} of 10 watcher callbacks were collected`);
  }
}

async function updateExcludedPathsBeforeStart() {
  const excluded = [path.join(watchDir, 'excluded')];
  const watch = await nsfw(watchDir, () => {});
  await watch.updateExcludedPaths(excluded);
  const stored = await watch.getExcludedPaths();
  if (JSON.stringify(stored) !== JSON.stringify(excluded)) {
    throw new Error(`excluded paths not kept: ${JSON.stringify(stored)}`);
  }
  await watch.start();
  await watch.stop();
}

// Many busy watchers whose excluded paths change while events flow: each platform watcher reads the list (and on
// Linux changes its watch tree) on its own thread while updateExcludedPaths replaces it from a worker thread.
async function updateExcludedPathsUnderLoad() {
  const excludedDirs = [];
  for (let i = 0; i < 20; i++) {
    excludedDirs.push(path.join(watchDir, `excluded${i}`));
    await fse.mkdirp(path.join(watchDir, `excluded${i}`, 'nested'));
  }

  const watches = [];
  for (let i = 0; i < 40; i++) {
    const watch = await nsfw(watchDir, () => {}, { debounceMS: 1 });
    await watch.start();
    watches.push(watch);
  }

  let writing = true;
  let writes = 0;
  const writer = (async () => {
    for (let i = 0; writing; i++) {
      const dir = path.join(excludedDirs[i % excludedDirs.length], 'nested', `dir${i % 5}`);
      try {
        await fse.mkdirp(dir);
        await fse.writeFile(path.join(dir, 'file'), String(i));
        await fse.writeFile(path.join(watchDir, `file${i % 20}`), String(i));
        await fse.remove(dir);
        writes++;
      } catch (error) {
        // Only here to keep the watchers busy: a transient failure (EBUSY or EPERM on Windows while something else
        // holds a file) must not end the child as an unhandled rejection before `await writer`.
      }
    }
  })();

  let excluded = [];
  const deadline = Date.now() + 5000;
  for (let i = 0; Date.now() < deadline; i++) {
    // Lists of changing length, so the native vectors also reallocate.
    excluded = i % 2 === 0 ? [] : excludedDirs.slice(i % excludedDirs.length);
    await Promise.all(watches.map(watch => watch.updateExcludedPaths(excluded)));
  }
  writing = false;
  await writer;
  if (writes === 0) {
    throw new Error('the writer never kept the watchers busy');
  }

  for (const watch of watches) {
    const stored = await watch.getExcludedPaths();
    if (JSON.stringify(stored) !== JSON.stringify(excluded)) {
      throw new Error(`excluded paths not kept: ${JSON.stringify(stored)}`);
    }
    await watch.stop();
  }
}

// Many busy watchers in a worker that is terminated while they run: the poll threads must be gone before the
// environment destroys the callbacks they use.
async function terminateBusyWorkers() {
  let counter = 0;
  for (let i = 0; i < 15; i++) {
    const worker = new Worker(__filename, { workerData: ['busy-watchers', watchDir] });
    await new Promise(resolve => worker.once('message', resolve));
    let writing = true;
    const writer = (async () => {
      while (writing) {
        await fse.writeFile(path.join(watchDir, `file${counter++ % 20}`), String(counter));
      }
    })();
    await sleep(20 + Math.floor(Math.random() * 60));
    await worker.terminate();
    writing = false;
    await writer;
  }
}

async function startBusyWatchers() {
  for (let i = 0; i < 40; i++) {
    const watch = await nsfw(watchDir, () => {}, { debounceMS: 1 });
    await watch.start();
  }
  parentPort.postMessage('started');
}

function errorInWorker() {
  return new Promise((resolve, reject) => {
    const worker = new Worker(__filename, { workerData: ['error', watchDir] });
    worker.on('error', reject);
    worker.on('exit', code => (code === 0 ? resolve() : reject(new Error(`worker exited with ${code}`))));
  });
}

const scenarios = {
  'stop': stopAndRestart,
  'ordered-jobs': orderedJobs,
  'terminate-queued': terminateQueuedWorkers,
  'queued-jobs': queueJobs,
  'restart-after-error': restartAfterError,
  'discard-stopped-error': discardStoppedRunError,
  'collect-errored': collectErroredWatchers,
  'concurrent-stop': concurrentStops,
  'error': errorWithoutStop,
  'collect-callbacks': collectCallbacks,
  'update-before-start': updateExcludedPathsBeforeStart,
  'update-excluded-under-load': updateExcludedPathsUnderLoad,
  'worker-terminate-busy': terminateBusyWorkers,
  'busy-watchers': startBusyWatchers,
  'worker-error': errorInWorker
};

// A pending promise does not keep the process alive, so a scenario that never settles would let the child exit with 0.
// The watchdog keeps it alive and fails it instead; it is cleared once the scenario settles, so anything the watcher
// leaves behind still keeps the child from exiting. A rejection exits with 1, and from a worker it reaches the parent.
const watchdog = setTimeout(() => {
  throw new Error(`scenario '${mode}' did not settle`);
}, ['worker-terminate-busy', 'update-excluded-under-load'].includes(mode) ? 55000 : 15000);
scenarios[mode]().then(() => clearTimeout(watchdog));
