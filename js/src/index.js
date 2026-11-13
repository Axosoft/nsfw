const { promises: fs } = require('fs');
const path = require('path');

const NSFW = require('../../build/Release/nsfw.node');

const isWin32 = process.platform === 'win32';

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

// Rewrites excludedPath as watchPath without trailing separators followed by the normalized part below it, or returns
// null when excludedPath is outside watchPath. The native watchers match that spelling: Linux builds its tree from
// watchPath as given and macOS moves paths under it onto its real path, so a path that is only inside after resolving
// `.` or `..` (/a/b/../w/x when watching /a/w) would otherwise be kept and never match.
// path.relative compares whole path components (so /a/foobar is not inside /a/foo) and handles win32
// case-insensitivity, mixed separators and other drives. Comparing the namespaced forms also counts a `\\?\` path on
// one side and a plain one on the other as inside on win32.
function rebaseOnWatchPath(watchPath, excludedPath) {
  const relativePath = path.relative(path.toNamespacedPath(watchPath), path.toNamespacedPath(excludedPath));
  if (relativePath === '..' || relativePath.startsWith(`..${path.sep}`) || path.isAbsolute(relativePath)) {
    return null;
  }
  const base = normalizeExcludedPath(watchPath);
  if (relativePath === '') {
    return base;
  }
  // Only a root keeps its trailing separator.
  const endsWithSeparator = base.endsWith('/') || (isWin32 && base.endsWith('\\'));
  return endsWithSeparator ? base + relativePath : base + path.sep + relativePath;
}

// Validates excludedPaths, drops the ones outside watchPath, which cannot exclude anything, and rebases the rest on
// watchPath. The filter is here and not in the native watchers, where an ancestor of watchPath would match every
// event on macOS and Windows.
function effectiveExcludedPaths(watchPath, excludedPaths) {
  validateExcludedPaths(excludedPaths);
  return excludedPaths
    .map(excludedPath => rebaseOnWatchPath(watchPath, excludedPath))
    .filter(excludedPath => excludedPath !== null);
}

// Mirrors normalizeExcludedPath in src/NSFW.cpp: drops trailing separators, but a root keeps its own.
function normalizeExcludedPath(excludedPath) {
  let normalized = excludedPath;
  while (normalized.length > 1) {
    const last = normalized[normalized.length - 1];
    const isSeparator = last === '/' || (isWin32 && last === '\\');
    const isDriveRoot = isWin32 && normalized[normalized.length - 2] === ':';
    if (!isSeparator || isDriveRoot) {
      break;
    }
    normalized = normalized.slice(0, -1);
  }
  return normalized;
}

function NSFWFilePoller(watchPath, eventCallback, debounceMS, excludedPaths) {
  const { CREATED, DELETED, MODIFIED } = nsfw.actions;
  const directory = path.dirname(watchPath);
  const file = path.basename(watchPath);

  let fileStatus;
  let filePollerInterval;
  // Nothing inside a single file can be excluded, so the list is only kept to be returned, like the native watcher's.
  let currentExcludedPaths = excludedPaths.map(normalizeExcludedPath);

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

  this.getExcludedPaths = async () => [...currentExcludedPaths];
  this.updateExcludedPaths = async (paths) => {
    currentExcludedPaths = paths.map(normalizeExcludedPath);
    return [...currentExcludedPaths];
  };
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
    return new NSFWFilePoller(watchPath, eventCallback, debounceMS, effectivePaths);
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
