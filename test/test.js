var nsfw = require("../index.js");
var assert = require("assert");
var fs = require("fs");
var path = require("path");

describe('Node Sentinel File Watcher', function() {
  var workDir = path.resolve("./mockfs");
  beforeEach(function() {
    function makeDir(identifier) {
      fs.mkdirSync(path.join(workDir, "test" + identifier));
      fs.mkdirSync(path.join(workDir, "test" + identifier, "folder" + identifier));
      var fd = fs.openSync(path.join(workDir, "test" + identifier, "testing" + identifier +".file"), "w");
      fs.writeSync(fd, "testing");
      fs.closeSync(fd);
    }
    // create a file System
    fs.mkdirSync(workDir);
    for (var i = 0; i < 5; ++i) {
      makeDir(i);
    }
  });

  afterEach(function() {
    fs.unlinkSync(workDir);
  });

  it('can listen for a create event', function() {

  });
});
