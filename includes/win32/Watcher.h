#ifndef NSFW_WIN32_WATCHER
#define NSFW_WIN32_WATCHER

#include <WinSock2.h>
#include <Windows.h>
#include <string>
#include <mutex>
#include <atomic>
#include <map>
#include <thread>
#include <vector>

#include "../SingleshotSemaphore.h"
#include "../PathFilter.h"
#include "../Queue.h"

class Watcher
{
  public:
    Watcher(std::shared_ptr<EventQueue> queue, const std::wstring &path,
      const std::vector<std::wstring> &excludedPaths, std::shared_ptr<PathFilter> filter);
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
    void handleArrival(const std::wstring &fileName);
    void handleRemoval(const std::wstring &fileName);
    void handleRename(const std::wstring &oldFileName, const std::wstring &newFileName);
    void flushPendingRename();
    bool isIgnoredDirectory(const std::wstring &fileName);
    void reportContents(const std::wstring &directory);
    bool consumeScanned(const std::wstring &fileName);
    void forgetScanned(const std::wstring &fileName);
    void forgetOldScans();
    void reopenWathedFolder();
    void openParent();
    void watchParent();
    void parentChanged(DWORD errorCode);
    HANDLE openDirectory(const std::wstring &path);

    void resizeBuffers(std::size_t size);

    std::string getUTF8Directory(std::wstring path) ;
    bool isExcluded(const std::wstring &fileName);
    bool isExcludedPath(const std::wstring &path);

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
    std::shared_ptr<PathFilter> mFilter;

    std::vector<BYTE> mReadBuffer, mWriteBuffer;
    // Reads aborted in a row while running. Only used on the watcher thread.
    unsigned mAbortedReads = 0;
    // The old name of a rename whose new name hasn't been read yet, empty if none. Only used on the watcher thread.
    std::wstring mPendingOldName;
    // The entries the scans of directories that appeared reported, by their lower-case path relative to the watched
    // one, with the number of the read they were found in: the system may report their creation too, see
    // forgetOldScans(). Only used on the watcher thread.
    std::map<std::wstring, uint64_t> mScannedEntries;
    uint64_t mReads = 0;
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
