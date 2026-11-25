#ifndef NSFW_WIN32_WATCHER
#define NSFW_WIN32_WATCHER

#include <WinSock2.h>
#include <Windows.h>
#include <string>
#include <mutex>
#include <atomic>
#include <thread>
#include <vector>

#include "../SingleshotSemaphore.h"
#include "../Queue.h"

class Watcher
{
  public:
    Watcher(std::shared_ptr<EventQueue> queue, const std::wstring &path,
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
    void eventCallback(DWORD errorCode, DWORD numBytes);
    void handleEvents();
    void reopenWathedFolder();
    void openParent();
    void watchParent();
    void parentChanged(DWORD errorCode);
    HANDLE openDirectory(const std::wstring &path);

    void resizeBuffers(std::size_t size);

    std::string getUTF8Directory(std::wstring path) ;
    bool isExcluded(const std::wstring &fileName);

    std::wstring getWatchedPathFromHandle();
    void checkWatchedPath();
    bool hasError();

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
    // Read on the watcher thread, replaced from a worker thread by updateExcludedPaths.
    std::mutex mExcludedPathsMutex;
    std::vector<std::wstring> mExcludedPaths;

    std::vector<BYTE> mReadBuffer, mWriteBuffer;
    // Reads aborted in a row while running. Only used on the watcher thread.
    unsigned mAbortedReads = 0;
    OVERLAPPED mOverlapped;

    // The watched directory's own watch reports nothing when it is renamed, its parent's does, see watchParent().
    HANDLE mParentHandle = INVALID_HANDLE_VALUE;
    std::vector<BYTE> mParentBuffer;
    OVERLAPPED mParentOverlapped;
    // Without the parent's notifications, the watcher thread compares the path each second instead.
    std::atomic<bool> mPollWatchedPath{false};

    std::thread mRunner;
};


#endif
