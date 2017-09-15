#include "../includes/Queue.h"


#include "../includes/NativeInterface.h"
#include "../includes/Listener.hpp"
#include "../includes/FileSystemWatcher.h"
#include <vector>
#include <iostream>
#include <regex>
#include <thread>

typedef std::unique_ptr<std::vector<std::unique_ptr<Event>>> VecEvents;

class TransformBase
{
public:
    VecEvents operator()(VecEvents vecEvents)
    {
        return transform(std::move(vecEvents));
    }

public:
    virtual VecEvents transform(VecEvents vecEvents) = 0;
};

class PrintOutTransform : public TransformBase
{
public:
    VecEvents transform(VecEvents vecEvents)
    {
        for (const auto& event : *vecEvents) {
            std::cout << event->type << ": "
                      << event->directory << " "
                      << event->fileA << " tp "
                      << event->timePoint.time_since_epoch().count()
                      << std::endl;
        }

        return vecEvents;
    }
};

class ExcludeDirectoryTransform : public TransformBase
{
public:
    ExcludeDirectoryTransform(const std::string &dir)
        : excludedDirectory(dir) {

    }

    VecEvents transform(VecEvents vecEvents)
    {
        vecEvents->erase(std::remove_if(vecEvents->begin(),
                                       vecEvents->end(),
                                       [this](std::unique_ptr<Event> &event){
            std::regex self_regex(excludedDirectory,
                                  std::regex_constants::ECMAScript
                                | std::regex_constants::icase);
            return std::regex_search(event->directory, self_regex);
        }), vecEvents->end());

        return vecEvents;
    }
private:
    std::string excludedDirectory;
};

class ExcludeFileTransform : public TransformBase
{
public:
    ExcludeFileTransform(const std::string &regex)
        : regexStr(regex) {

    }

    VecEvents transform(VecEvents vecEvents)
    {
        vecEvents->erase(std::remove_if(vecEvents->begin(),
                                       vecEvents->end(),
                                       [this](std::unique_ptr<Event> &event){
            std::regex self_regex(regexStr,
                                  std::regex_constants::ECMAScript
                                | std::regex_constants::icase);
            return std::regex_search(event->fileA, self_regex);
        }), vecEvents->end());

        return vecEvents;
    }
private:
    std::string regexStr;
};

int main() {

    std::vector<std::unique_ptr<TransformBase>> vec;
    std::unique_ptr<TransformBase> exDir(new ExcludeDirectoryTransform("dir"));
    vec.push_back(std::move(exDir));
    std::unique_ptr<TransformBase> exFile(new ExcludeFileTransform(".txt"));
    vec.push_back(std::move(exFile));
    std::unique_ptr<TransformBase> printer(new PrintOutTransform());
    vec.push_back(std::move(printer));

    NodeSentinalFileWatcher::FileSystemWatcher watcher("path", std::chrono::milliseconds(50));

    watcher.registerCallback([&](VecEvents events) {
        for (auto& transformer : vec) {
            events = (*transformer)(std::move(events));
        }
    });

    while(true);

}
