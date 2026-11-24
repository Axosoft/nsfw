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

// Once per process, as Node does for its own deprecations.
let warnedExcludedPaths = false;
function warnExcludedPaths() {
  if (warnedExcludedPaths) return;
  warnedExcludedPaths = true;
  process.emitWarning(
    'excludedPaths, updateExcludedPaths() and getExcludedPaths() are deprecated: use excludedPatterns, ' +
    'updateExcludedPatterns() and getExcludedPatterns() instead (\'/build\' excludes build in the watched directory).',
    { type: 'DeprecationWarning', code: 'NSFW_EXCLUDED_PATHS' });
}

function validateExcludedPatterns(excludedPatterns) {
  if (!Array.isArray(excludedPatterns) || excludedPatterns.some(pattern => typeof pattern !== 'string')) {
    throw new TypeError('excludedPatterns must be an array of strings.');
  }
}

const isExtendedUnc = value => value.slice(0, 8).toLowerCase() === '\\\\?\\unc\\';

function stripTrailingSeparators(value) {
  const rootLength = isWin32 && isExtendedUnc(value) ?
    path.parse('\\\\' + value.slice(8)).root.length + 6 : path.parse(value).root.length;
  while (value.length > rootLength && (value.endsWith('/') || (isWin32 && value.endsWith('\\')))) {
    value = value.slice(0, -1);
  }
  return value;
}

async function canonicalPath(value) {
  if (isWin32) {
    if (isExtendedUnc(value)) {
      value = '\\\\' + value.slice(8);
    } else if (value.startsWith('\\\\?\\') && /^[a-z]:/i.test(value.slice(4))) {
      value = value.slice(4);
    }
    return stripTrailingSeparators(path.toNamespacedPath(path.resolve(value)));
  }
  // realpath must see symlink/.. before any lexical normalization.
  return fs.realpath(stripTrailingSeparators(value));
}

const pathComponents = value => value.split('/').filter(component => component !== '' && component !== '.');

// realpath(3) can resolve `..` as text, also inside symlink targets, without the search permission check of the
// kernel's own lookup. A spelling it returns only counts when that lookup reaches the same file.
async function isSameFile(lookup, physical) {
  const [found, expected] = await Promise.all([fs.stat(lookup, { bigint: true }), fs.stat(physical, { bigint: true })]);
  return found.dev === expected.dev && found.ino === expected.ino;
}

// Plain children stay lexical so they may name future paths or internal symlinks. A prefix containing `..`
// must pass one cumulative kernel lookup, preserving search permissions and the symlink traversal limit.
async function createPosixExclusionResolver(watchPath, canonicalWatchPath) {
  const canonicalRoot = pathComponents(canonicalWatchPath);
  const knownRoots = [pathComponents(watchPath), canonicalRoot];
  const lexicalRoot = path.resolve(watchPath);
  if (lexicalRoot !== canonicalWatchPath) {
    try {
      if (await fs.realpath(lexicalRoot) === canonicalWatchPath && await isSameFile(lexicalRoot, canonicalWatchPath)) {
        knownRoots.push(pathComponents(lexicalRoot));
      }
    } catch (error) { /* A collapsed spelling need not exist. */ }
  }
  const childPrefix = canonicalWatchPath === '/' ? '/' : canonicalWatchPath + '/';
  const isInside = value => value === canonicalWatchPath || value.startsWith(childPrefix);

  return async value => {
    let components = pathComponents(value);
    const knownRoot = knownRoots.find(root => root.every((component, index) => component === components[index]));
    if (knownRoot !== undefined) {
      // Frozen root aliases remain bound to the original target even after a symlink is repointed.
      components = [...canonicalRoot, ...components.slice(knownRoot.length)];
    }
    const lastParent = components.lastIndexOf('..');
    if (knownRoot !== undefined && lastParent < canonicalRoot.length) {
      return '/' + components.join('/');
    }

    // Unknown aliases are probed cumulatively until their spelling enters the watched namespace. Never
    // rebuild a later lookup from realpath's answer: that would reset the kernel's traversal budget.
    for (let end = Math.max(1, lastParent + 1); end <= components.length; end++) {
      const lookup = '/' + components.slice(0, end).join('/');
      let physical;
      try {
        physical = await fs.realpath(lookup);
        if (!await isSameFile(lookup, physical)) return null;
      } catch (error) {
        return null;
      }
      if (isInside(physical)) {
        return path.join(physical, ...components.slice(end));
      }
    }
    return null;
  };
}

// The directory the native watchers reported for the watched path before they received canonical paths.
function legacyDirectory(watchPath) {
  if (!isWin32) return watchPath !== '/' && watchPath.endsWith('/') ? watchPath.slice(0, -1) : watchPath;
  if (watchPath.startsWith('\\\\?\\')) return watchPath;
  const resolved = path.resolve(watchPath);
  return resolved + (/[\\/]$/.test(watchPath) && !resolved.endsWith(path.sep) ? path.sep : '');
}

// Native events carry canonical directories, while Linux and Windows callers keep receiving the legacy spelling.
// Returns undefined when no directory needs rewriting.
function createDirectoryMapper(watchPath, canonicalWatchPath, canonicalChildPrefix, contains) {
  if (process.platform === 'darwin') return undefined;
  const reportedBase = legacyDirectory(watchPath);
  const reportedParent = stripTrailingSeparators(reportedBase);
  const reportedChildPrefix = isWin32 ? reportedParent + (reportedParent.endsWith(path.sep) ? '' : path.sep) :
    reportedBase + (watchPath === '/' ? '' : '/');
  if (reportedBase === canonicalWatchPath && reportedChildPrefix === canonicalChildPrefix) return undefined;
  return value => {
    if (!contains(value)) return value;
    const suffix = value.slice(canonicalChildPrefix.length);
    return suffix ? reportedChildPrefix + suffix : reportedBase;
  };
}

async function createPathContext(watchPath) {
  const canonicalWatchPath = await canonicalPath(watchPath);
  if (!isWin32 && !await isSameFile(watchPath, canonicalWatchPath)) throw new Error('Not the watched path.');
  const compare = value => isWin32 ? value.toLowerCase() : value;
  const base = compare(canonicalWatchPath);
  const canonicalChildPrefix = canonicalWatchPath.endsWith(path.sep) ? canonicalWatchPath :
    canonicalWatchPath + path.sep;
  const prefix = compare(canonicalChildPrefix);
  const contains = value => {
    const compared = compare(value);
    return compared === base || compared.startsWith(prefix);
  };
  // Win32 itself resolves `..` as a string, before any reparse point.
  const excludedPath = isWin32 ? canonicalPath : await createPosixExclusionResolver(watchPath, canonicalWatchPath);

  return {
    watchPath: canonicalWatchPath,
    directory: createDirectoryMapper(watchPath, canonicalWatchPath, canonicalChildPrefix, contains),
    isWatchPath: value => compare(value) === base,
    async excludedPaths(paths) {
      validateExcludedPaths(paths);
      return (await Promise.all(paths.map(excludedPath))).filter(value => value !== null && contains(value));
    }
  };
}

function NSFWFilePoller(watchPath, eventCallback, debounceMS, excludedPaths, reportedPath, isExcluded,
  excludedPatterns) {
  const { CREATED, DELETED, MODIFIED } = nsfw.actions;
  const directory = path.dirname(reportedPath);
  const file = path.basename(reportedPath);

  let fileStatus;
  let filePollerInterval;

  let currentExcludedPaths = excludedPaths;
  // Kept, though only a watched directory applies them.
  let currentExcludedPatterns = excludedPatterns;

  const report = events => { if (!currentExcludedPaths.some(isExcluded)) eventCallback(events); };

  const getStatus = async () => {
    try {
      const status = await fs.stat(watchPath);
      if (fileStatus === null) {
        fileStatus = status;
        report([{ action: CREATED, directory, file }]);
      } else if (
        status.mtime - fileStatus.mtime !== 0 ||
        status.ctime - fileStatus.ctime !== 0
      ) {
        fileStatus = status;
        report([{ action: MODIFIED, directory, file }]);
      }
    } catch (e) {
      if (fileStatus !== null) {
        fileStatus = null;
        report([{ action: DELETED, directory, file }]);
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
    currentExcludedPaths = paths;
    return [...currentExcludedPaths];
  };

  this.getExcludedPatterns = async () => [...currentExcludedPatterns];
  this.updateExcludedPatterns = async (patterns) => {
    currentExcludedPatterns = patterns;
    return [...currentExcludedPatterns];
  };
}

const buildNSFW = async (watchPath, eventCallback, {
  debounceMS = 500,
  errorCallback: _errorCallback,
  excludedPaths,
  excludedPatterns = [],
  respectGitignore = false
} = {}) => {
  if (excludedPaths === undefined) {
    excludedPaths = [];
  } else {
    warnExcludedPaths();
  }
  if (Number.isInteger(debounceMS)) {
    if (debounceMS < 1) {
      throw new Error('Minimum debounce is 1ms.');
    }
  } else {
    throw new Error('debounceMS must be an integer.');
  }

  validateExcludedPatterns(excludedPatterns);
  if (typeof respectGitignore !== 'boolean') {
    throw new TypeError('respectGitignore must be a boolean.');
  }

  const errorCallback = _errorCallback || ((nsfwError) => { throw nsfwError; });

  if (!path.isAbsolute(watchPath)) {
    throw new Error('Path to watch must be an absolute path.');
  }

  let context, stats;
  try {
    context = await createPathContext(watchPath);
    // Node's metadata APIs reject extended drive roots that the native watcher can open.
    const metadataPath = isWin32 && /^\\\\\?\\[A-Za-z]:\\$/.test(context.watchPath) ?
      context.watchPath.slice(4) : context.watchPath;
    stats = await fs.stat(metadataPath);
  } catch (e) {
    throw new Error('Path must be a valid path to a file or a directory.');
  }

  if (typeof eventCallback !== 'function') throw new TypeError('eventCallback must be a function.');

  const effectivePaths = await context.excludedPaths(excludedPaths);
  const callback = context.directory === undefined ? eventCallback : events => eventCallback(events.map(event => ({
    ...event,
    directory: context.directory(event.directory),
    ...(event.newDirectory === undefined ? {} : { newDirectory: context.directory(event.newDirectory) })
  })));
  let implementation;

  if (stats.isDirectory()) {
    implementation = new NSFW(context.watchPath, callback,
      {
        debounceMS,
        errorCallback,
        excludedPaths: effectivePaths,
        excludedPatterns: [...excludedPatterns],
        respectGitignore
      });
  } else if (stats.isFile()) {
    implementation = new NSFWFilePoller(context.watchPath, eventCallback, debounceMS, effectivePaths,
      watchPath, context.isWatchPath, [...excludedPatterns]);
  } else {
    throw new Error('Path must be a valid path to a file or a directory');
  }
  return { implementation, context };
};

function nsfw(watchPath, eventCallback, options) {
  if (!(this instanceof nsfw)) {
    return buildNSFW(watchPath, eventCallback, options).then(result => new nsfw(result));
  }

  // Constructed by the branch above.
  const { implementation, context } = watchPath;

  // Calls reach the watcher in call order. Canonicalizing an update can wait for the filesystem, so a call made
  // while an earlier one has not reached the watcher yet waits behind it; any other call goes straight to the
  // watcher, which creates its job synchronously.
  let waiting = 0;
  let updates = Promise.resolve();
  const whenReady = call => {
    if (waiting === 0) return call();
    waiting++;
    return updates.then(() => {
      waiting--;
      return call();
    });
  };
  const inOrder = method => () => whenReady(() => implementation[method]());
  this.start = inOrder('start');
  this.stop = inOrder('stop');
  this.pause = inOrder('pause');
  this.resume = inOrder('resume');
  const getExcludedPaths = inOrder('getExcludedPaths');
  this.getExcludedPaths = () => {
    warnExcludedPaths();
    return getExcludedPaths();
  };
  this.getExcludedPatterns = inOrder('getExcludedPatterns');
  this.updateExcludedPatterns = async patterns => {
    validateExcludedPatterns(patterns);
    const snapshot = [...patterns];
    return whenReady(() => implementation.updateExcludedPatterns(snapshot));
  };
  this.updateExcludedPaths = async paths => {
    warnExcludedPaths();
    validateExcludedPaths(paths);
    const snapshot = [...paths];
    waiting++;
    const update = updates.then(async () => {
      let canonicalPaths;
      try {
        canonicalPaths = await context.excludedPaths(snapshot);
      } finally {
        waiting--;
      }
      // The native FIFO orders execution; waiting for completion here would keep stop()
      // from reaching an update that needs to yield its unfinished scan to shutdown.
      return { result: implementation.updateExcludedPaths(canonicalPaths) };
    });
    updates = update.catch(() => {});
    return (await update).result;
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
