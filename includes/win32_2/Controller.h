#ifndef NSFW_WIN32_CONTROLLER
#define NSFW_WIN32_CONTROLLER

//#include "../Queue.h"
#include <string>
#include <memory>
#include "Watcher.h"

class EventQueue;

class Controller {
  public:
    Controller(EventQueue &queue, const std::string &path);

    std::string getError();
    bool hasErrored();
    bool isWatching();

    ~Controller();
  private:
    std::unique_ptr<Watcher> mWatcher;
//  protected:
//    void run();
//    void shutdown();
//    std::shared_ptr<ReadLoopRunner> *mRunner;
//    std::wstring mDirectory;
//    HANDLE mDirectoryHandle;
//    EventQueue &mQueue;
//  private:
//    unsigned int mThreadID;
//    HANDLE mThread;
//    bool mRunning;


};

#endif
