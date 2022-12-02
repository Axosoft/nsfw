const {
  isMainThread,
  parentPort,
  workerData
} = require('worker_threads');

const nsfw = require('../src');

if (isMainThread) {
  throw new Error('Must be run via worker thread');
}


const { workDir, test } = workerData;

// aggressively collects garbage until we fail to improve terminatingIteration times.
function garbageCollect() {
  const terminatingIterations = 3;
  let usedBeforeGC = Number.MAX_VALUE;
  let nondecreasingIterations = 0;
  for (; ;) {
    global.gc();
    const usedAfterGC = process.memoryUsage().heapUsed;
    if (usedAfterGC >= usedBeforeGC) {
      nondecreasingIterations++;
      if (nondecreasingIterations >= terminatingIterations) {
        break;
      }
    }
    usedBeforeGC = usedAfterGC;
  }
}


parentPort.postMessage('init');

const runTest1 = async () => {
  const watch = await nsfw(
    workDir,
    () => {}
  );
  await watch.start();
  parentPort.postMessage('success');
};


const runTest2 = async () => {
  const watch1 = await nsfw(
    workDir,
    () => {}
  );
  await watch1.start();

  const watch2 = await nsfw(
    workDir,
    () => {}
  );
  await watch2.start();

  let watch3 = await nsfw(
    workDir,
    () => {}
  );
  await watch3.start();
  await watch3.stop();
  watch3 = null;
  garbageCollect();
  parentPort.postMessage('success');
};

switch(test) {
  case 1:
    runTest1().then(() => {});
    break;
  case 2:
    runTest2().then(() => {});
    break;
}
