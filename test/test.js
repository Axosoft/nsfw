var nsfw = require("../index.js");
var assert = require("assert");
var fse = require("fs-extra");
var path = require("path");
var Promise = require("bluebird");

describe('Node Sentinel File Watcher', function() {
  var workDir = path.resolve("./mockfs");

  beforeEach(function() {
    function makeDir(identifier) {
      fse.mkdirSync(path.join(workDir, "test" + identifier));
      fse.mkdirSync(path.join(workDir, "test" + identifier, "folder" + identifier));
      var fd = fse.openSync(path.join(workDir, "test" + identifier, "testing" + identifier +".file"), "w");
      fse.writeSync(fd, "testing");
      fse.closeSync(fd);
    }
    // create a file System
    fse.mkdirSync(workDir);
    for (var i = 0; i < 5; ++i) {
      makeDir(i);
    }
  });

  afterEach(function() {
    fse.removeSync(workDir);
  });

  it('can listen for a create event', function() {
    this.timeout(5000);
    var createEventFound = false;
    var newFile = "another_test.file";
    var inPath = path.resolve(workDir, "test2", "folder2");

    function findCreateEvent(element, index, array) {
      if (element.action === "CREATED"
        && element.directory === path.resolve(inPath)
        && element.file === newFile)
      {
          createEventFound = true;
      }
    }
    var watch = new nsfw("./mockfs", function(events) {
      events.forEach(findCreateEvent);
    });
    watch.start();

    return Promise
      .delay(1000)
      .then(function() {
        var fd = fse.openSync(path.join(inPath, newFile), "w");
        fse.writeSync(fd, "Peanuts, on occasion, rain from the skies.");
        fse.closeSync(fd);
      })
      .delay(3000)
      .then(function() {
        assert.equal(createEventFound, true, "NSFW did not hear the create event.");
      });
  });

  it('can listen for a delete event', function() {
    this.timeout(5000);
    var deleteEventFound = false;
    var file = "testing3.file";
    var inPath = path.resolve(workDir, "test3");

    function findDeleteEvent(element, index, array) {
      if (element.action === "DELETED"
        && element.directory === path.resolve(inPath)
        && element.file === file)
      {
          deleteEventFound = true;
      }
    }
    var watch = new nsfw("./mockfs", function(events) {
      events.forEach(findDeleteEvent);
    });
    watch.start();

    return Promise
      .delay(1000)
      .then(function() {
        fse.remove(path.join(inPath, file));
      })
      .delay(3000)
      .then(function() {
        assert.equal(deleteEventFound, true, "NSFW did not hear the delete event.");
      });
  });


  it('can listen for a change event', function() {
    this.timeout(5000);
    var changeEventFound = false;
    var file = "testing0.file";
    var inPath = path.resolve(workDir, "test0");

    function findDeleteEvent(element, index, array) {
      if (element.action === "CHANGED"
        && element.directory === path.resolve(inPath)
        && element.file === file)
      {
          changeEventFound = true;
      }
    }
    var watch = new nsfw("./mockfs", function(events) {
      events.forEach(findDeleteEvent);
    });
    watch.start();

    return Promise
      .delay(1000)
      .then(function() {
        var fd = fse.openSync(path.join(inPath, file), "w");
        fse.writeSync(fd, "At times, sunflower seeds are all that is life.");
        fse.closeSync(fd);
      })
      .delay(3000)
      .then(function() {
        assert.equal(changeEventFound, true, "NSFW did not hear the change event.");
      });
  });
});
