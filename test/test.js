var nsfw = require("../index.js");
var assert = require("assert");
var fse = require("fs-extra");
var path = require("path");
var Promise = require("bluebird");
nsfw.prototype.stop = Promise.promisify(nsfw.prototype.stop);

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
      var errors = false;

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

      var watch = new nsfw(
        filePath,
        function(events) {
          events.forEach(findEvents);
        },
        function() {
          error = true;
        }
      );
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
          assert(error == false, "NSFW received an error.");
          assert(changeEvents >= 1, "NSFW did not hear the change event.");
          assert(createEvents == 2, "NSFW did not hear the create event.");
          assert(deleteEvents == 2, "NSFW did not hear the delete event.");
          return watch.stop();
        })
        .catch(function(error) {
          return watch.stop()
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
      var errors = false;

      function findCreateEvent(element, index, array) {
        if (element.action === "CREATED"
          && element.directory === path.resolve(inPath)
          && element.file === newFile)
        {
            createEventFound = true;
        }
      }
      var watch = new nsfw(
        "./mockfs",
        function(events) {
          events.forEach(findCreateEvent);
        },
        function() {
          errors = true;
        }
      );
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
          assert(error == false, "NSFW received an error.");
          assert.equal(createEventFound, true, "NSFW did not hear the create event.");
          return watch.stop();
        })
        .catch(function(error) {
          return watch.stop()
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
      var errors = false;

      function findDeleteEvent(element, index, array) {
        if (element.action === "DELETED"
          && element.directory === path.resolve(inPath)
          && element.file === file)
        {
            deleteEventFound = true;
        }
      }
      var watch = new nsfw(
        "./mockfs",
        function(events) {
          events.forEach(findDeleteEvent);
        },
        function() {
          errors = true;
        }
      );
      watch.start();

      return Promise
        .delay(1000)
        .then(function() {
          fse.remove(path.join(inPath, file));
        })
        .delay(2000)
        .then(function() {
          assert(error == false, "NSFW received an error.");
          assert.equal(deleteEventFound, true, "NSFW did not hear the delete event.");
          return watch.stop();
        })
        .catch(function(error) {
          return watch.stop()
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
      var errors = false;

      function findChangeEvent(element, index, array) {
        if (element.action === "CHANGED"
          && element.directory === path.resolve(inPath)
          && element.file === file)
        {
            changeEventFound = true;
        }
      }
      var watch = new nsfw(
        "./mockfs",
        function(events) {
          events.forEach(findChangeEvent);
        },
        function() {
          errors = true;
        }
      );
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
          assert(errors == false, "NSFW received an error.");
          assert.equal(changeEventFound, true, "NSFW did not hear the change event.");
          return watch.stop();
        })
        .catch(function(error) {
          return watch.stop()
          .then(function() {
            Promise.reject(error);
          });
        });
    });

    it('can run multiple watchers at once', function() {
      this.timeout(5000);
      var deleteEvents = 0;
      var dirA = path.resolve(workDir, "test0");
      var fileA = "testing1.file";
      var dirB = path.resolve(workDir, "test1");
      var fileB = "testing0.file";
      var errors = false;

      function registerCreateEvents(element, index, array) {
        if (element.action === "CREATED") {
          if (element.directory === dirA && element.file === fileA) {
            deleteEvents++;
          } else if (element.directory === dirB && element.file === fileB) {
            deleteEvents++;
          }
        }
      }

      function eventHandler(events) {
        events.forEach(registerCreateEvents);
      }

      var watchA = new nsfw(
        dirA,
        eventHandler,
        function() {
          errors = true;
        }
      );
      var watchB = new nsfw(
        dirB,
        eventHandler,
        function() {
          errors = true;
        }
      );

      watchA.start();
      watchB.start();

      return Promise
        .delay(1000)
        .then(function() {
          var fd = fse.openSync(path.join(dirA, fileA), "w");
          fse.writeSync(fd, "Peanuts, on occasion, rain from the skies.");
          fse.closeSync(fd);

          fd = fse.openSync(path.join(dirB, fileB), "w");
          fse.writeSync(fd, "Peanuts, on occasion, rain from the skies.");
          fse.closeSync(fd);
        })
        .delay(2000)
        .then(function() {
          assert(errors == false, "NSFW received an error.");
          assert.equal(deleteEvents, 2, "Failed to hear both delete events.");
          return watchA.stop();
        })
        .then(function() {
          return watchB.stop();
        })
        //.then(function() {})
        .catch(function(error) {
          return watchA.stop()
            .then(function() {
              return watchB.stop();
            })
            .catch(function() {
              return watchB.stop();
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
      var errors = false;

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
      var watch = new nsfw(
        "./mockfs",
        function(events) {
          events.forEach(findCreateEvent);
        },
        function() {
          errors = true;
        }
      );
      watch.start();

      return Promise
        .delay(1000)
        .then(function() {
          fse.copySync(inPath, outPath);
        })
        .delay(2000)
        .then(function() {
          assert(errors == false, "NSFW received an error.");
          assert.equal(createdCount, 3, "NSFW did not hear all 3 delete events.");
          return watch.stop();
        })
        .catch(function(error) {
          return watch.stop()
          .then(function(error) {
            Promise.reject(error);
          });
        });
    });

    it('can listen for the destruction of directory and its subtree', function() {
      this.timeout(5000);
      var inPath = path.resolve(workDir, "test4");
      var deletionCount = 0;
      var errors = false;

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
      var watch = new nsfw(
        "./mockfs",
        function(events) {
          events.forEach(findDeleteEvent);
        },
        function() {
          errors = true;
        }
      );
      watch.start();

      return Promise
        .delay(1000)
        .then(function() {
          fse.remove(inPath);
        })
        .delay(2000)
        .then(function() {
          assert(errors == false, "NSFW received an error.");
          assert.equal(deletionCount, 3, "NSFW did not hear all 3 delete events.");
          return watch.stop();
        })
        .catch(function(error) {
          return watch.stop()
          .then(function() {
            Promise.reject(error);
          });
        });
    });
  });

  describe('Errors', function() {
    it('can gracefully recover when the watch folder is deleted', function() {
      this.timeout(5000);
      var inPath = path.resolve(workDir, "test4");
      var errorFound = false;

      var watch = new nsfw(
        inPath,
        function(){},
        function(error) {
          if (error.message === "Access is denied") {
            errorFound = true;
          }
        }
      );

      watch.start();

      return Promise
        .delay(500)
        .then(function() {
          fse.removeSync(inPath);
        })
        .delay(3000)
        .then(function() {
          assert.equal(errorFound, true, "NSFW did not throw an exception when the watch folder was deleted.");
        });
    });

    it('does not segfault after creating/destroying watches repeatedly', function() {
      this.timeout(21000);
      var watch = new nsfw(workDir, function(){}, function(e) { throw e; });

      var counter = 10;

      function startAndStop() {
        return Promise.resolve()
          .then(function() {
            watch.start();
          })
          .delay(Math.floor((Math.random() * 2000) + 1))
          .then(function() {
            return watch.stop();
          })
          .then(function() {
            if (counter > 0) {
              counter--;
              return startAndStop();
            }
          });
      };

      return startAndStop();
    });
  });
});
