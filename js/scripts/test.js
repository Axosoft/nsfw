const cp = require('child_process');

// NSFW_TEST_SUITE=slow|normal runs one suite, so CI can run them in parallel jobs; unset runs both.
const suite = process.env.NSFW_TEST_SUITE || 'all';
if (!['all', 'slow', 'normal'].includes(suite)) {
  throw new Error(`Unknown NSFW_TEST_SUITE: ${suite}`);
}

// The specs load js/dist, so they never run against a stale build.
require('./build');
exec('tsc -p js/spec/types');

// One file at a time: the specs share the mockfs work directory.
const nodeTest = 'node --test --test-concurrency=1 --test-force-exit --expose-gc --test-reporter=spec';

if (process.platform !== 'win32' && process.platform !== 'darwin') {
  // When ran as a npm script we can invoke npm bins such as node-gyp directly
  if (suite !== 'normal') {
    exec('node-gyp rebuild', { env: { ...process.env, NSFW_TEST_SLOW: 1 } });
    exec(`${nodeTest} js/spec/index-slow-spec.js js/spec/slow/*-spec.js`);
  }
  exec('node-gyp rebuild');
}
if (suite !== 'slow') {
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
