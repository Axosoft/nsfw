#define CATCH_CONFIG_RUNNER
#include "catch.hpp"

#include <limits.h>
#include <functional>
#include <algorithm>
#include <string>

std::string programmName;
int main( int argc, char* argv[] )
{
  programmName = argv[0];
  int result = Catch::Session().run( argc, argv );

  return ( result < 0xff ? result : 0xff );
}

#include "../includes/FileSystemWatcher.h"
#include "../includes/transforms/AbstractTransform.h"

#include "test_helper.h"

std::string getDirectoryFromFile(const std::string &path)
{
    char delimiter;

#if defined(_WIN32)
    delimiter = '\\';
#elif defined(__linux__)
    delimiter = '/';
#endif
    return std::string(path.c_str(), path.find_last_of(delimiter));
}

class TestFileSystemAdapter
{
public:
    TestFileSystemAdapter(const std::string &path, std::chrono::milliseconds duration)
        : fswatch(path, duration)
    {
        vecEvents.reset(new VecEvents::element_type());
        fswatch.registerCallback(std::bind(&TestFileSystemAdapter::listernerFunction, this, std::placeholders::_1));
    }

    VecEvents getEventsAfterWait(std::chrono::microseconds ms)
    {
        std::this_thread::sleep_for(ms);
        std::lock_guard<std::mutex> lock(VecEvents);
        auto retVal = std::move(vecEvents);
        vecEvents.reset(new VecEvents::element_type());
        return retVal;
    }

private:
    void listernerFunction(VecEvents events)
    {
        std::lock_guard<std::mutex> lock(VecEvents);
        if (!events) return;
        for(auto &event : *events) {
            vecEvents->push_back(std::move(event));
        }

    }

    std::mutex vecEventsMutex;
    VecEvents  vecEvents;
    NodeSentinalFileWatcher::FileSystemWatcher fswatch;

};


TEST_CASE( "test the file system watcher", "[FileSystemWatcher]" )
{

    std::vector<std::unique_ptr<AbstractTransform>> vec;

    std::string executionPath(getDirectoryFromFile(programmName));
    TestFileSystemAdapter testWatcher(executionPath, std::chrono::milliseconds(10));
    auto comparation = [] (const Event &lhs, const Event &rhs) {
        return lhs.type == rhs.type &&
               lhs.directory == rhs.directory &&
               lhs.fileA == rhs.fileA &&
               lhs.fileB == rhs.fileB;
    };

    auto eventWasDetected = [comparation] (TestFileSystemAdapter &testWatcher, const Event &expectedEvent) -> bool {
        auto events = testWatcher.getEventsAfterWait(std::chrono::milliseconds(20));

        bool foundEvent{false};
        for(auto &event : *events) {
            if (comparation(*event, expectedEvent)) {
                foundEvent = true;
            }
        }

        return foundEvent;
    };

    SECTION("check file creation")
    {
        std::string fileName("testFile");
        DummyFile fileHandle(fileName);
        Event expectedEvent(EventType::CREATED, executionPath, fileName);

        REQUIRE( eventWasDetected(testWatcher, expectedEvent) );
    }

    SECTION("check file modification")
    {
        std::string fileName("testFile");
        std::string payload("payload");
        DummyFile fileHandle(fileName);
        Event expectedEvent(EventType::MODIFIED, executionPath, fileName);

        // call this without expectations because a normal file creation s a CREATION and MODIFY
        eventWasDetected(testWatcher, expectedEvent);

        fileHandle.modify(payload);

        REQUIRE( eventWasDetected(testWatcher, expectedEvent) );
    }

    SECTION("check file renaming")
    {
        std::string fileName("testFile");
        std::string newFileName("newFileName");
        DummyFile fileHandle(fileName);
        Event expectedEvent(EventType::RENAMED, executionPath, fileName, newFileName);

        fileHandle.rename(newFileName);

        REQUIRE( eventWasDetected(testWatcher, expectedEvent) );
    }

    SECTION("check file deletion")
    {
        std::string fileName("testFile");
        std::unique_ptr<DummyFile> fileHandle(new DummyFile(fileName));
        Event expectedEvent(EventType::DELETED, executionPath, fileName);

        fileHandle.reset(nullptr);

        REQUIRE( eventWasDetected(testWatcher, expectedEvent) );
    }
}
