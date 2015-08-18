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

  describe('Basic Functions', function() {
    it('can watch a single file', function() {
      this.timeout(10000);
      var file = "testing1.file";
      var inPath = path.resolve(workDir, "test1");
      var filePath = path.join(inPath, file);
      var changeEvents = 0;
      var createEvents = 0;
      var deleteEvents = 0;

      function findEvents(element, index, array) {
        if (element.action === "CHANGED"
          && element.directory === path.resolve(inPath)
          && element.file === file)
        {
            changeEvents++;
        }
        else if (element.action === "CREATED"
          && element.directory === path.resolve(inPath)
          && element.file === file)
        {
            createEvents++;
        }
        else if (element.action === "DELETED"
          && element.directory === path.resolve(inPath)
          && element.file === file)
        {
            deleteEvents++;
        }
      }

      var watch = new nsfw(filePath, function(events) {
        events.forEach(findEvents);
      });
      watch.start();

      return Promise
        .delay(1000)
        .then(function() {
          var fd = fse.openSync(filePath, "w");
          fse.writeSync(fd, "Bean bag video games at noon.");
          fse.closeSync(fd);
        })
        .delay(500)
        .then(function(){
          fse.removeSync(filePath);
        })
        .delay(500)
        .then(function(){
          fd = fse.openSync(filePath, "w");
          fse.writeSync(fd, "His watch has ended.");
          fse.closeSync(fd);
        })
        .delay(500)
        .then(function(){
          fse.renameSync(filePath, path.join(inPath, "not_test.file"));
        })
        .delay(500)
        .then(function(){
          fse.renameSync(path.join(inPath, "not_test.file"), filePath);
        })
        .delay(1500)
        .then(function() {
          assert(changeEvents >= 1, "NSFW did not hear the change event.");
          assert(createEvents == 2, "NSFW did not hear the create event.");
          assert(deleteEvents == 2, "NSFW did not hear the delete event.");
          return Promise.promisify(watch.stop, watch)();
        })
        .catch(function(error) {
          return Promise.promisify(watch.stop, watch)()
          .then(function() {
            Promise.reject(error);
          });
        });
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
        .delay(2000)
        .then(function() {
          assert.equal(createEventFound, true, "NSFW did not hear the create event.");
          return Promise.promisify(watch.stop, watch)();
        })
        .catch(function(error) {
          return Promise.promisify(watch.stop, watch)()
          .then(function() {
            Promise.reject(error);
          });
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
        .delay(2000)
        .then(function() {
          assert.equal(deleteEventFound, true, "NSFW did not hear the delete event.");
          return Promise.promisify(watch.stop, watch)();
        })
        .catch(function(error) {
          return Promise.promisify(watch.stop, watch)()
          .then(function() {
            Promise.reject(error);
          });
        });
    });


    it('can listen for a change event', function() {
      this.timeout(5000);
      var changeEventFound = false;
      var file = "testing0.file";
      var inPath = path.resolve(workDir, "test0");

      function findChangeEvent(element, index, array) {
        if (element.action === "CHANGED"
          && element.directory === path.resolve(inPath)
          && element.file === file)
        {
            changeEventFound = true;
        }
      }
      var watch = new nsfw("./mockfs", function(events) {
        events.forEach(findChangeEvent);
      });
      watch.start();

      return Promise
        .delay(1000)
        .then(function() {
          var fd = fse.openSync(path.join(inPath, file), "w");
          fse.writeSync(fd, "At times, sunflower seeds are all that is life.");
          fse.closeSync(fd);
        })
        .delay(2000)
        .then(function() {
          assert.equal(changeEventFound, true, "NSFW did not hear the change event.");
          return Promise.promisify(watch.stop, watch)();
        })
        .catch(function(error) {
          return Promise.promisify(watch.stop, watch)()
          .then(function() {
            Promise.reject(error);
          });
        });
    });

    it('can run multiple watchers at once', function() {
      this.timeout(5000);
      var deleteEvents = 0;
      var dirA = path.resolve(workDir, "test0");
      var fileA = "testing0.file";
      var dirB = path.resolve(workDir, "test1");
      var fileB = "testing1.file";

      function registerDeleteEvents(element, index, array) {
        if (element.action === "DELETED") {
          if (element.directory === dirA && element.file === fileA) {
            deleteEvents++;
          } else if (element.directory === dirB && element.file === fileB) {
            deleteEvents++;
          }
        }
      }

      function eventHandler(events) {
        events.forEach(registerDeleteEvents);
      }

      var watchA = new nsfw(dirA, eventHandler);
      var watchB = new nsfw(dirB, eventHandler);

      watchA.start();
      watchB.start();

      return Promise
        .delay(1000)
        .then(function() {
          fse.removeSync(path.join(dirA, fileA));
          fse.removeSync(path.join(dirB, fileB));
        })
        .delay(2000)
        .then(function() {
          assert.equal(deleteEvents, 2, "Failed to hear both delete events.");
          return Promise.promisify(watchA.stop, watchA);
        })
        .then(function() {
          return Promise.promisify(watchB.stop, watchB);
        })
        .catch(function(error) {
          return Promise.promisify(watchA.stop, watchA)()
            .then(function() {
              return Promise.promisify(watchB.stop, watchB)()
            })
            .then(function() {
              Promise.reject(error);
            });
        });
    });
  });
  describe("Recursive Functions", function() {
    it('can listen for the creation of directory and its subtree', function() {
      this.timeout(5000);
      var inPath = path.resolve(workDir, "test1");
      var outPath = path.resolve(workDir, "test6");
      var createdCount = 0;

      function findCreateEvent(element, index, array) {
        if (element.action === "CREATED")
        {
          if (element.directory === outPath
              && (element.file === "testing1.file" || element.file === "folder1"))
          {
            createdCount++;
          }
          else if (element.directory === workDir && element.file === "test6")
          {
            createdCount++;
          }
        }
      }
      var watch = new nsfw("./mockfs", function(events) {
        events.forEach(findCreateEvent);
      });
      watch.start();

      return Promise
        .delay(1000)
        .then(function() {
          fse.copySync(inPath, outPath);
        })
        .delay(2000)
        .then(function() {
          assert.equal(createdCount, 3, "NSFW did not hear all 3 delete events.");
          return Promise.promisify(watch.stop, watch)();
        })
        .catch(function(error) {
          return Promise.promisify(watch.stop, watch)()
          .then(function(error) {
            Promise.reject(error);
          });
        });
    });

    it('can listen for the destruction of directory and its subtree', function() {
      this.timeout(5000);
      var inPath = path.resolve(workDir, "test4");
      var deletionCount = 0;

      function findDeleteEvent(element, index, array) {
        if (element.action === "DELETED")
        {
          if (element.directory === path.resolve(inPath)
              && (element.file === "testing4.file" || element.file === "folder4"))
          {
            deletionCount++;
          }
          else if (element.directory === workDir && element.file === "test4")
          {
            deletionCount++;
          }
        }
      }
      var watch = new nsfw("./mockfs", function(events) {
        events.forEach(findDeleteEvent);
      });
      watch.start();

      return Promise
        .delay(1000)
        .then(function() {
          fse.remove(inPath);
        })
        .delay(2000)
        .then(function() {
          assert.equal(deletionCount, 3, "NSFW did not hear all 3 delete events.");
          return Promise.promisify(watch.stop, watch)();
        })
        .catch(function(error) {
          return Promise.promisify(watch.stop, watch)()
          .then(function() {
            Promise.reject(error);
          });
        });
    });
  });
});
