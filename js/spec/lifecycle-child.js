/* global FinalizationRegistry */
// Runs in a child process (and, for 'worker-error', in a worker thread inside it) so that a watcher that keeps the
// event loop alive, or aborts during teardown, cannot hang or crash the mocha process.
const { Worker, isMainThread, parentPort, workerData } = require('worker_threads');
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
  'concurrent-stop': concurrentStops,
  'error': errorWithoutStop,
  'collect-callbacks': collectCallbacks,
  'update-before-start': updateExcludedPathsBeforeStart,
  'worker-terminate-busy': terminateBusyWorkers,
  'busy-watchers': startBusyWatchers,
  'worker-error': errorInWorker
};

// A pending promise does not keep the process alive, so a scenario that never settles would let the child exit with 0.
// The watchdog keeps it alive and fails it instead; it is cleared once the scenario settles, so anything the watcher
// leaves behind still keeps the child from exiting. A rejection exits with 1, and from a worker it reaches the parent.
const watchdog = setTimeout(() => {
  throw new Error(`scenario '${mode}' did not settle`);
}, mode === 'worker-terminate-busy' ? 55000 : 15000);
scenarios[mode]().then(() => clearTimeout(watchdog));
