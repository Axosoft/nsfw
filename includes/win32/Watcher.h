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
    Watcher(std::shared_ptr<EventQueue> queue, const std::wstring &path, bool pathWasNtPrefixed,
      const std::vector<std::wstring> &excludedPaths);
    ~Watcher();

    bool isRunning() const { return mRunning; }
    std::string getError();
    void updateExcludedPaths(const std::vector<std::wstring> &excludedPaths);

  private:
    void run();
    bool pollDirectoryChanges();
    void start();
    void stop();

    void setError(const std::string &error);
    void eventCallback(DWORD errorCode);
    void handleEvents();
    void reopenWathedFolder();
    HANDLE openDirectory(const std::wstring &path);

    void resizeBuffers(std::size_t size);

    std::string getUTF8Directory(std::wstring path) ;
    bool Watcher::isExcluded(const std::wstring &fileName);

    std::wstring Watcher::getWatchedPathFromHandle();
    void Watcher::checkWatchedPath();

    std::atomic<bool> mRunning;
    SingleshotSemaphore mHasStartedSemaphore;
    SingleshotSemaphore mIsRunningSemaphore;
    mutable std::mutex mErrorMutex;
    mutable std::mutex mHandleMutex;
    std::string mError;
    std::wstring mWatchedPath;

    const std::wstring mPath;
    std::shared_ptr<EventQueue> mQueue;
    HANDLE mDirectoryHandle;
    bool mPathWasNtPrefixed;
    std::vector<std::wstring> mExcludedPaths;

    std::vector<BYTE> mReadBuffer, mWriteBuffer;
    OVERLAPPED mOverlapped;

    std::thread mRunner;
};


#endif
