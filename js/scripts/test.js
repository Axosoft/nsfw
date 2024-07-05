const cp = require('child_process');

if (process.platform !== 'win32' && process.platform !== 'darwin') {
  // When ran as a npm script we can invoke npm bins such as node-gyp and mocha directly
  exec('node-gyp rebuild', { env: { ...process.env, NSFW_TEST_SLOW: 1 } });
  exec('mocha --exit --v8-expose-gc js/spec/index-slow-spec.js');
  exec('node-gyp rebuild');
}
exec('mocha --exit --v8-expose-gc js/spec/index-spec.js');

/**
 * @param {string} commandline ...
 * @param {cp.ExecSyncOptions} options ...
 * @returns {Promise<void>} ...
 */
function exec(commandline, options = {}) {
  cp.execSync(commandline, { stdio: 'inherit', ...options });
}
