const cp = require('child_process');

// The specs load js/dist, so they never run against a stale build.
require('./build');
exec('tsc -p js/spec/types');

// One file at a time: the specs share the mockfs work directory.
const nodeTest = 'node --test --test-concurrency=1 --test-force-exit --expose-gc --test-reporter=spec';

if (process.platform !== 'win32' && process.platform !== 'darwin') {
  // When ran as a npm script we can invoke npm bins such as node-gyp directly
  exec('node-gyp rebuild', { env: { ...process.env, NSFW_TEST_SLOW: 1 } });
  exec(`${nodeTest} js/spec/index-slow-spec.js js/spec/slow/*-spec.js`);
  exec('node-gyp rebuild');
}
exec(`${nodeTest} js/spec/canonical-path-spec.js js/spec/index-spec.js js/spec/lifecycle-spec.js`);

/**
 * @param {string} commandline ...
 * @param {cp.ExecSyncOptions} options ...
 * @returns {Promise<void>} ...
 */
function exec(commandline, options = {}) {
  cp.execSync(commandline, { stdio: 'inherit', ...options });
}
