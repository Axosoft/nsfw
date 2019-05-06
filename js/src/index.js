const { NSFW } = require('../../build/Release/nsfw.node');
const fs = require('fs');
const path = require('path');
const { promisify } = require('util');

const stat = promisify(fs.stat);

const _private = {};

function nsfw() {
  if (!(this instanceof nsfw)) {
    return _private.buildNSFW(...arguments);
  }

  const _nsfw = new NSFW(...arguments);

  this.start = function start() {
    return new Promise((resolve, reject) => {
      _nsfw.start(err => {
        if (err) {
          reject(err);
        }
        resolve();
      });
    });
  };

  this.stop = function stop() {
    return new Promise(resolve => {
      _nsfw.stop(resolve);
    });
  };
}

nsfw.actions = {
  CREATED: 0,
  DELETED: 1,
  MODIFIED: 2,
  RENAMED: 3
};

_private.buildNSFW = function buildNSFW(watchPath, eventCallback, options) {
  let { debounceMS, errorCallback } = options || {};

  if (Number.isInteger(debounceMS)) {
    if (debounceMS < 1) {
      throw new Error('Minimum debounce is 1ms.');
    }
  } else if (debounceMS === undefined) {
    debounceMS = 500;
  } else {
    throw new Error('Option debounceMS must be a positive integer greater than 1.');
  }

  if (errorCallback === undefined) {
    errorCallback = function(nsfwError) {
      throw nsfwError;
    };
  }

  if (!path.isAbsolute(watchPath)) {
    throw new Error('Path to watch must be an absolute path.');
  }

  return stat(watchPath)
    .then(stats => {
      if (stats.isDirectory()) {
        return new nsfw(debounceMS, watchPath, eventCallback, errorCallback);
      } else if (stats.isFile()) {
        return new _private.nsfwFilePoller(debounceMS, watchPath, eventCallback);
      } else {
        throw new Error('Path must be a valid path to a file or a directory.');
      }
    }, () => {
      throw new Error('Path must be a valid path to a file or a directory.');
    });
};



_private.nsfwFilePoller = function(debounceMS, watchPath, eventCallback) {
  const { CREATED, DELETED, MODIFIED } = nsfw.actions;
  const directory = path.dirname(watchPath);
  const file = path.basename(watchPath);

  let fileStatus;
  let filePollerInterval;

  function getStatus() {
    return stat(watchPath)
      .then(status => {
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
      }, () => {
        if (fileStatus !== null) {
          fileStatus = null;
          eventCallback([{ action: DELETED, directory, file }]);
        }
      });
  }

  this.start = function start() {
    return stat(watchPath)
      .then(status => fileStatus = status, () => fileStatus = null)
      .then(() => {
        filePollerInterval = setInterval(getStatus, debounceMS);
      });
  };

  this.stop = function stop() {
    return Promise.resolve()
      .then(() => clearInterval(filePollerInterval));
  };
};

module.exports = nsfw;
