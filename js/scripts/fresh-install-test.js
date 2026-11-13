/* eslint-disable no-console */
// Installs the packed package into an empty project without a lockfile, the way a consumer runs `npm install nsfw`,
// so every dependency range resolves to its latest match instead of the versions pinned by yarn.lock. Then loads the
// addon and watches a directory, to prove that it compiled against those versions and works.
const cp = require('child_process');
const fs = require('fs');
const os = require('os');
const path = require('path');

const repoRoot = path.resolve(__dirname, '..', '..');
const timeoutMS = 30000;

// The addon is loaded in a child process: Windows can't delete a loaded nsfw.node, so the parent couldn't clean up.
const run = process.argv[2] === '--watch' ? watch(process.argv[3], process.argv[4]) : main();
run.catch(error => {
  console.error(error);
  process.exit(1);
});

async function main() {
  const workDir = fs.mkdtempSync(path.join(os.tmpdir(), 'nsfw-fresh-install-'));
  try {
    const projectDir = path.join(workDir, 'project');
    const watchDir = path.join(workDir, 'watched');
    fs.mkdirSync(projectDir);
    fs.mkdirSync(watchDir);
    console.log(`node ${process.version}, npm ${exec('npm --version', repoRoot).trim()}, project in ${projectDir}`);

    // The destination goes through the environment, so no path has to be quoted for the shell, and the tarball is
    // the only file in projectDir, so its name doesn't have to be parsed from npm's output.
    exec('npm pack', repoRoot, { stdio: 'inherit', env: { ...process.env, npm_config_pack_destination: projectDir } });
    const tarball = fs.readdirSync(projectDir).find(name => name.endsWith('.tgz'));
    if (!tarball) {
      throw new Error(`npm pack wrote no tarball to ${projectDir}`);
    }

    fs.writeFileSync(path.join(projectDir, 'package.json'), JSON.stringify({
      name: 'nsfw-fresh-install',
      version: '1.0.0',
      private: true,
      dependencies: { nsfw: `file:${tarball}` },
      // npm 12 skips dependency install scripts unless allowed here, and matches a local tarball by its exact spec.
      // Older npm ignores the field and always runs them.
      allowScripts: { [`file:${tarball}`]: true },
      devDependencies: selectedNodeGyp()
    }, null, 2));

    exec('npm install --no-package-lock --no-audit --no-fund --foreground-scripts', projectDir, { stdio: 'inherit' });
    if (fs.existsSync(path.join(projectDir, 'package-lock.json'))) {
      throw new Error('npm wrote a lockfile');
    }

    // The timeout also covers a watcher.stop() that never returns, which the child's own timeout can't.
    cp.execFileSync(process.execPath, [__filename, '--watch', projectDir, watchDir], {
      stdio: 'inherit',
      timeout: 2 * timeoutMS
    });
  } finally {
    // A failed cleanup must not hide the result, e.g. a scanner still holding nsfw.node on Windows.
    try {
      fs.rmSync(workDir, { recursive: true, force: true, maxRetries: 5 });
    } catch (error) {
      console.warn(`could not remove ${workDir}: ${error.message}`);
    }
  }
}

/**
 * Loads nsfw from projectDir and waits for a watcher on watchDir to report a new file.
 * @param {string} projectDir ...
 * @param {string} watchDir ...
 * @returns {Promise<void>} ...
 */
async function watch(projectDir, watchDir) {
  const resolve = request => require.resolve(request, { paths: [projectDir] });
  const nodeAddonApiVersion = require(resolve('node-addon-api/package.json')).version;
  console.log(`installed nsfw ${require(resolve('nsfw/package.json')).version}, node-addon-api ${nodeAddonApiVersion}`);

  // A missing binary, because the install script was skipped or failed quietly, throws here.
  const nsfw = require(resolve('nsfw'));
  let onEvent;
  const sawFile = new Promise(resolvePromise => {
    onEvent = events => {
      if (events.some(event => event.file === 'created.txt')) {
        resolvePromise();
      }
    };
  });
  const watcher = await nsfw(watchDir, onEvent, { debounceMS: 100 });
  await watcher.start();
  try {
    fs.writeFileSync(path.join(watchDir, 'created.txt'), 'nsfw');
    await withTimeout(sawFile, `no event for created.txt within ${timeoutMS}ms`);
  } finally {
    await watcher.stop();
  }
  console.log('fresh install works: the watcher reported the new file');
}

/**
 * npm ignores npm_config_node_gyp in dependency install scripts: arborist doesn't pass it on, so @npmcli/run-script
 * points it back at the node-gyp bundled with npm, which for npm 10 predates Visual Studio 2026. That PATH puts the
 * project's node_modules/.bin first, so the project depends on the node-gyp the environment selected and npm runs it.
 * @returns {object} A devDependencies entry for that node-gyp, or nothing when npm_config_node_gyp is unset.
 */
function selectedNodeGyp() {
  if (!process.env.npm_config_node_gyp) {
    return {};
  }
  const packageJson = path.join(path.dirname(process.env.npm_config_node_gyp), '..', 'package.json');
  return { 'node-gyp': require(packageJson).version };
}

/**
 * Runs a command through the shell, which Windows needs to find npm.cmd.
 * @param {string} commandline ...
 * @param {string} cwd ...
 * @param {object} [options] Extra execSync options, such as stdio or env.
 * @returns {string} The standard output, or an empty string when it is inherited.
 */
function exec(commandline, cwd, options = {}) {
  return cp.execSync(commandline, { cwd, encoding: 'utf8', stdio: ['ignore', 'pipe', 'inherit'], ...options }) || '';
}

/**
 * @param {Promise<void>} promise ...
 * @param {string} message ...
 * @returns {Promise<void>} ...
 */
function withTimeout(promise, message) {
  let timer;
  const timeout = new Promise((_, reject) => {
    timer = setTimeout(() => reject(new Error(message)), timeoutMS);
  });
  return Promise.race([promise, timeout]).finally(() => clearTimeout(timer));
}
