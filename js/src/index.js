const { promises: fs } = require('fs');
const path = require('path');

const NSFW = require('../../build/Release/nsfw.node');

// Throws unless excludedPaths is an array of absolute paths.
function validateExcludedPaths(excludedPaths) {
  if (!Array.isArray(excludedPaths)) {
    throw new TypeError('excludedPaths must be an array of strings.');
  }
  for (const excludedPath of excludedPaths) {
    if (typeof excludedPath !== 'string') {
      throw new TypeError('excludedPaths elements must be strings.');
    }
    if (!path.isAbsolute(excludedPath)) {
      throw new Error('Excluded path must be an absolute path.');
    }
  }
}

// Whether excludedPath is watchPath or inside it. path.relative compares whole path components (so /a/foobar is not
// inside /a/foo) and handles win32 case-insensitivity, mixed separators and other drives. Comparing the namespaced
// forms also counts a `\\?\` path on one side and a plain one on the other as inside on win32, which the native
// watcher handles by adding that prefix to every path.
function isInsideWatchPath(watchPath, excludedPath) {
  const relativePath = path.relative(path.toNamespacedPath(watchPath), path.toNamespacedPath(excludedPath));
  return relativePath !== '..' && !relativePath.startsWith(`..${path.sep}`) && !path.isAbsolute(relativePath);
}

// Validates excludedPaths and drops the ones outside watchPath, which cannot exclude anything. It is done here and not
// in the native watchers, where an ancestor of watchPath would match every event on macOS and Windows.
function effectiveExcludedPaths(watchPath, excludedPaths) {
  validateExcludedPaths(excludedPaths);
  return excludedPaths.filter(excludedPath => isInsideWatchPath(watchPath, excludedPath));
}

function NSFWFilePoller(watchPath, eventCallback, debounceMS) {
  const { CREATED, DELETED, MODIFIED } = nsfw.actions;
  const directory = path.dirname(watchPath);
  const file = path.basename(watchPath);

  let fileStatus;
  let filePollerInterval;

  const getStatus = async () => {
    try {
      const status = await fs.stat(watchPath);
      if (fileStatus === null) {
        fileStatus = status;
        eventCallback([{ action: CREATED, directory, file }]);
      } else if (
        status.mtime - fileStatus.mtime !== 0 ||
        status.ctime - fileStatus.ctime !== 0
      ) {
        fileStatus = status;
        eventCallback([{ action: MODIFIED, directory, file }]);
      }
    } catch (e) {
      if (fileStatus !== null) {
        fileStatus = null;
        eventCallback([{ action: DELETED, directory, file }]);
      }
    }
  };

  this.start = async () => {
    try {
      fileStatus = await fs.stat(watchPath);
    } catch (e) {
      fileStatus = null;
    }

    filePollerInterval = setInterval(getStatus, debounceMS);
  };

  this.stop = async () => {
    clearInterval(filePollerInterval);
  };

  this.pause = () => this.stop();
  this.resume = () => this.start();
}

const buildNSFW = async (watchPath, eventCallback,
  { debounceMS = 500, errorCallback: _errorCallback, excludedPaths = [] } = {}) => {
  if (Number.isInteger(debounceMS)) {
    if (debounceMS < 1) {
      throw new Error('Minimum debounce is 1ms.');
    }
  } else {
    throw new Error('debounceMS must be an integer.');
  }

  const errorCallback = _errorCallback || ((nsfwError) => { throw nsfwError; });

  if (!path.isAbsolute(watchPath)) {
    throw new Error('Path to watch must be an absolute path.');
  }

  let stats;
  try {
    stats = await fs.stat(watchPath);
  } catch (e) {
    throw new Error('Path must be a valid path to a file or a directory.');
  }

  const effectivePaths = effectiveExcludedPaths(watchPath, excludedPaths);

  if (stats.isDirectory()) {
    return new NSFW(watchPath, eventCallback, { debounceMS, errorCallback, excludedPaths: effectivePaths });
  } else if (stats.isFile()) {
    return new NSFWFilePoller(watchPath, eventCallback, debounceMS);
  } else {
    throw new Error('Path must be a valid path to a file or a directory');
  }
};

function nsfw(watchPath, eventCallback, options) {
  if (!(this instanceof nsfw)) {
    return buildNSFW(watchPath, eventCallback, options).then(implementation => new nsfw(implementation, watchPath));
  }

  // Constructed by the branch above.
  const [implementation, watchedPath] = arguments;

  this.start = () => implementation.start();
  this.stop = () => implementation.stop();
  this.pause = () => implementation.pause();
  this.resume = () => implementation.resume();
  this.getExcludedPaths = () => implementation.getExcludedPaths();
  this.updateExcludedPaths = async (paths) => {
    return implementation.updateExcludedPaths(effectiveExcludedPaths(watchedPath, paths));
  };
}


nsfw.actions = {
  CREATED: 0,
  DELETED: 1,
  MODIFIED: 2,
  RENAMED: 3
};

nsfw._native = NSFW;

if (NSFW.getAllocatedInstanceCount) {
  nsfw.getAllocatedInstanceCount = NSFW.getAllocatedInstanceCount;
}

module.exports = nsfw;
