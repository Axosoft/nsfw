const { promises: fs } = require('fs');
const path = require('path');

const NSFW = require('../../build/Release/nsfw.node');

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
        status.mtime.getTime() !== fileStatus.mtime.getTime() ||
        status.ctime.getTime() !== fileStatus.ctime.getTime()
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

const buildNSFW = async (
  watchPath,
  eventCallback,
  { debounceMS = 500, errorCallback: _errorCallback, excludedPaths = [] } = {}
) => {
  if (Number.isInteger(debounceMS)) {
    if (debounceMS < 1 || debounceMS > 60000) {
      throw new Error('debounceMS must be >= 1 and <= 60000.');
    }
  } else {
    throw new Error('debounceMS must be an integer.');
  }

  const errorCallback =
    _errorCallback ||
    ((nsfwError) => {
      throw nsfwError;
    });

  if (!path.isAbsolute(watchPath)) {
    throw new Error('Path to watch must be an absolute path.');
  }

  let stats;
  try {
    stats = await fs.stat(watchPath);
  } catch (e) {
    throw new Error('Path must be a valid path to a file or a directory.');
  }

  if (excludedPaths) {
    const normalizedWatchPath = watchPath.replace(/[\\/]+$/, '');
    for (let i = 0; i < excludedPaths.length; i++) {
      const normalizedExcludedPath = excludedPaths[i].replace(/[\\/]+$/, '');
      if (process.platform === 'win32') {
        if (
          normalizedExcludedPath
            .substring(0, normalizedWatchPath.length)
            .localeCompare(normalizedWatchPath, undefined, {
              sensitivity: 'accent',
            }) !== 0
        ) {
          throw new Error(
            'Excluded path must be a valid subdirectory of the watching path.'
          );
        }
      } else {
        if (!normalizedExcludedPath.startsWith(normalizedWatchPath)) {
          throw new Error(
            'Excluded path must be a valid subdirectory of the watching path.'
          );
        }
      }
    }
  }

  if (stats.isDirectory()) {
    return new NSFW(watchPath, eventCallback, {
      debounceMS,
      errorCallback,
      excludedPaths,
    });
  } else if (stats.isFile()) {
    return new NSFWFilePoller(watchPath, eventCallback, debounceMS);
  } else {
    throw new Error('Path must be a valid path to a file or a directory');
  }
};

function nsfw(watchPath, eventCallback, options) {
  if (!(this instanceof nsfw)) {
    return buildNSFW(watchPath, eventCallback, options).then(
      (implementation) => new nsfw(implementation)
    );
  }

  const implementation = watchPath;

  this.start = () => implementation.start();
  this.stop = () => implementation.stop();
  this.pause = () => implementation.pause();
  this.resume = () => implementation.resume();
  this.getExcludedPaths = () =>
    implementation.getExcludedPaths
      ? implementation.getExcludedPaths()
      : Promise.resolve([]);
  this.updateExcludedPaths = (paths) =>
    implementation.updateExcludedPaths
      ? implementation.updateExcludedPaths(paths)
      : Promise.resolve();
}

nsfw.actions = {
  CREATED: 0,
  DELETED: 1,
  MODIFIED: 2,
  RENAMED: 3,
};

nsfw._native = NSFW;

if (NSFW.getAllocatedInstanceCount) {
  nsfw.getAllocatedInstanceCount = NSFW.getAllocatedInstanceCount;
}

module.exports = nsfw;
