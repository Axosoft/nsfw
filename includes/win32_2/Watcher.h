#ifndef NSFW_WIN32_WATCHER
#define NSFW_WIN32_WATCHER

#include <WinSock2.h>
#include <Windows.h>
#include <string>
#include <mutex>
#include <atomic>
#include <thread>

#include "../SingleshotSemaphore.h"
#include "../Queue.h"

class Watcher
{
  public:
    Watcher(EventQueue &queue, HANDLE dirHandle, const std::wstring &path);
    ~Watcher();

    bool isRunning() const { return mRunning; }
    std::string getError() const;

  private:
    void run();

    void start();
    void stop();

    void setError(const std::string &error);

    std::atomic<bool> mRunning;
    SingleshotSemaphore mHasStartedSemaphore;
    mutable std::mutex mErrorMutex;
    std::string mError;

    const std::wstring mPath;
    EventQueue mQueue;
    HANDLE mDirectoryHandle;

    std::thread mRunner;
};


#endif
