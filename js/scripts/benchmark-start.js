// Measures how long start() takes on a directory tree and how much memory the watcher holds once started.
// Usage: node --expose-gc js/scripts/benchmark-start.js <path> [runs]
const path = require('path');
const { performance } = require('perf_hooks');
const nsfw = require('../src/index');

const gc = global.gc || (() => {});

async function measure(targetPath) {
  const watcher = await nsfw(targetPath, () => {}, { debounceMS: 500, errorCallback: (error) => { throw error; } });
  gc();
  const rssBefore = process.memoryUsage().rss;
  const startedAt = performance.now();
  await watcher.start();
  const startMS = performance.now() - startedAt;
  gc();
  const rssDelta = process.memoryUsage().rss - rssBefore;
  const stoppedAt = performance.now();
  await watcher.stop();
  const stopMS = performance.now() - stoppedAt;
  return { startMS, stopMS, rssDelta };
}

async function main() {
  const [targetArg, runsArg = '5'] = process.argv.slice(2);
  if (!targetArg) {
    throw new Error('Usage: node --expose-gc js/scripts/benchmark-start.js <path> [runs]');
  }
  const targetPath = path.resolve(targetArg);
  const runs = Number.parseInt(runsArg, 10);
  // The first run is the only one whose memory the process hadn't taken already. Its time counts a cold page cache.
  const { rssDelta } = await measure(targetPath);
  const results = [];
  for (let i = 0; i < runs; ++i) {
    results.push(await measure(targetPath));
  }
  const median = (key) => results.map((result) => result[key]).sort((a, b) => a - b)[Math.floor(runs / 2)];
  console.log(`start_ms=${median('startMS').toFixed(1)}`);
  console.log(`stop_ms=${median('stopMS').toFixed(1)}`);
  console.log(`rss_delta_mb=${(rssDelta / 1024 / 1024).toFixed(1)}`);
  console.log(`max_rss_mb=${(process.resourceUsage().maxRSS / 1024).toFixed(1)}`);
}

main().catch((error) => {
  console.error(error);
  process.exit(1);
});
