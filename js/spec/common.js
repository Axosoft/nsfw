const path = require('path');
const util = require('util');

exports.DEBOUNCE = 1000;
exports.TIMEOUT_PER_STEP = 3000;
exports.WORKDIR = path.resolve(__dirname, '../../mockfs');

exports.sleep = util.promisify(setTimeout);
