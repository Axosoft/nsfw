const cp = require('child_process');
const fs = require('fs');

// NSFW_TEST_SUITE=slow|normal runs one suite, so CI can run them in parallel jobs; unset runs both. slow-1, slow-2 and
// slow-3 split the slow suite into parts that take about as long, so CI can run those in parallel too.
const suite = process.env.NSFW_TEST_SUITE || 'all';
const slowParts = [
  ['js/spec/slow/restoration-spec.js'],
  ['js/spec/slow/aliases-spec.js']
];
// The last part takes the rest, so a new spec file runs without being listed.
slowParts.push(['js/spec/index-slow-spec.js', ...fs.readdirSync('js/spec/slow')
  .filter(file => file.endsWith('-spec.js'))
  .map(file => `js/spec/slow/${file}`)
  .filter(file => !slowParts.flat().includes(file))]);
const slowSuites = slowParts.map((_, index) => `slow-${index + 1}`);
if (!['all', 'slow', 'normal', ...slowSuites].includes(suite)) {
  throw new Error(`Unknown NSFW_TEST_SUITE: ${suite}`);
}
const runsSlow = suite !== 'normal';
const runsNormal = suite === 'all' || suite === 'normal';
const slowFiles = slowSuites.includes(suite) ? slowParts[slowSuites.indexOf(suite)] : slowParts.flat();

// The specs load js/dist, so they never run against a stale build.
require('./build');
exec('tsc -p js/spec/types');

// One file at a time: the specs share the mockfs work directory.
const nodeTest = 'node --test --test-concurrency=1 --test-force-exit --expose-gc --test-reporter=spec';

if (process.platform !== 'win32' && process.platform !== 'darwin') {
  // When ran as a npm script we can invoke npm bins such as node-gyp directly
  if (runsSlow) {
    exec('node-gyp rebuild', { env: { ...process.env, NSFW_TEST_SLOW: 1 } });
    exec(`${nodeTest} ${slowFiles.join(' ')}`);
  }
  exec('node-gyp rebuild');
}
if (runsNormal) {
  exec(`${nodeTest} js/spec/canonical-path-spec.js js/spec/index-spec.js js/spec/lifecycle-spec.js`);
}

/**
 * @param {string} commandline ...
 * @param {cp.ExecSyncOptions} options ...
 * @returns {Promise<void>} ...
 */
function exec(commandline, options = {}) {
  cp.execSync(commandline, { stdio: 'inherit', ...options });
}
