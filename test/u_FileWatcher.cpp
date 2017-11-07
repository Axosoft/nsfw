#define CATCH_CONFIG_RUNNER
#include "catch.hpp"

#include <algorithm>
#include <functional>
#include <limits.h>
#include <string>

static std::string programmName;
int main(int argc, char *argv[]) {
  programmName = argv[0];
  int result = Catch::Session().run(argc, argv);

  return (result < 0xff ? result : 0xff);
}

#include "nsfw/FileSystemWatcher.h"
#include "nsfw/transforms/AbstractTransform.h"

#include "test_helper.h"

using namespace NSFW;

#if defined(_WIN32)
static constexpr const char delimiter = '\\';
#elif defined(__linux__)
static constexpr const char delimiter = '/';
#endif

static std::string getDirectoryFromFile(const std::string &path) {
  return std::string(path.c_str(), path.find_last_of(delimiter));
}

static std::string getFileNameFromFile(const std::string &path) {
  auto firstCharFromFileName = path.find_last_of(delimiter) + 1;
  return std::string(path.c_str() + firstCharFromFileName,
                     path.size() - firstCharFromFileName);
}

class TestFileSystemAdapter {
public:
  TestFileSystemAdapter(const std::string &path,
                        std::chrono::milliseconds duration)
      : fswatch(path, duration) {
    vecEvents.reset(new VecEvents::element_type());
    fswatch.registerCallback(
        std::bind(&TestFileSystemAdapter::listernerFunction, this,
                  std::placeholders::_1));
  }

  VecEvents getEventsAfterWait(std::chrono::microseconds ms) {
    std::this_thread::sleep_for(ms);
    std::lock_guard<std::mutex> lock(VecEvents);
    auto retVal = std::move(vecEvents);
    vecEvents.reset(new VecEvents::element_type());
    return retVal;
  }

private:
  void listernerFunction(VecEvents events) {
    std::lock_guard<std::mutex> lock(VecEvents);
    if (!events)
      return;
    for (auto &event : *events) {
      vecEvents->push_back(std::move(event));
    }
  }

  std::mutex vecEventsMutex;
  VecEvents vecEvents;
  NSFW::FileSystemWatcher fswatch;
};

TEST_CASE("test the file system watcher", "[FileSystemWatcher]") {
  std::vector<std::unique_ptr<AbstractTransform>> vec;

  std::string tmpFilePath = std::tmpnam(nullptr);
  std::string executionPath(getDirectoryFromFile(tmpFilePath));
  TestFileSystemAdapter testWatcher(executionPath,
                                    std::chrono::milliseconds(10));
  auto comparison = [](const Event &lhs, const Event &rhs) {
    return lhs.type == rhs.type && lhs.directory == rhs.directory &&
           lhs.fileA == rhs.fileA && lhs.fileB == rhs.fileB;
  };

  auto eventWasDetected = [comparison](TestFileSystemAdapter &testWatcher,
                                        const Event &expectedEvent) -> bool {
    auto events = testWatcher.getEventsAfterWait(std::chrono::milliseconds(20));

    bool foundEvent{false};
    for (auto &event : *events) {
      if (comparison(*event, expectedEvent)) {
        foundEvent = true;
      }
    }

    return foundEvent;
  };

  SECTION("check file creation") {
    std::string filePath = std::tmpnam(nullptr);
    DummyFile fileHandle(filePath);
    Event expectedEvent(EventType::CREATED, executionPath,
                        getFileNameFromFile(filePath), "");

    REQUIRE(eventWasDetected(testWatcher, expectedEvent));
  }

  SECTION("check file modification") {
    std::string filePath = std::tmpnam(nullptr);
    std::string payload("payload");
    DummyFile fileHandle(filePath);
    Event expectedEvent(EventType::MODIFIED, executionPath,
                        getFileNameFromFile(filePath), "");

    // call this without expectations because a normal file creation s a
    // CREATION and MODIFY
    eventWasDetected(testWatcher, expectedEvent);

    fileHandle.modify(payload);

    REQUIRE(eventWasDetected(testWatcher, expectedEvent));
  }

  SECTION("check file renaming") {
    std::string filePath = std::tmpnam(nullptr);
    std::string newFilePath = std::tmpnam(nullptr);
    DummyFile fileHandle(filePath);
    Event expectedEvent(EventType::RENAMED, executionPath,
                        getFileNameFromFile(filePath),
                        getFileNameFromFile(newFilePath));

    fileHandle.rename(newFilePath);

    REQUIRE(eventWasDetected(testWatcher, expectedEvent));
  }

  SECTION("check file deletion") {
    std::string filePath = std::tmpnam(nullptr);
    std::unique_ptr<DummyFile> fileHandle(new DummyFile(filePath));
    Event expectedEvent(EventType::DELETED, executionPath,
                        getFileNameFromFile(filePath), "");

    fileHandle.reset(nullptr);

    REQUIRE(eventWasDetected(testWatcher, expectedEvent));
  }
}
