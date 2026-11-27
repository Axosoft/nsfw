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
      const std::vector<std::wstring> &excludedPaths, std::shared_ptr<PathFilter> filter, bool recursive = true);
    ~Watcher();

    bool isRunning() const { return mRunning; }
    std::string getError();
    void updateExcludedPaths(const std::vector<std::wstring> &excludedPaths);
    // Waits for the watcher thread to handle the reads that completed before this call, see the definition.
    void flush();
#ifdef NSFW_TEST_FAULTS_1
    // The bytes the read buffers of all watchers hold.
    static std::size_t testBufferBytes() { return sTestBufferBytes; }
#endif

  private:
    void run();
    bool pollDirectoryChanges();
    DWORD readDirectoryChanges();
    void start();
    void stop();

    void setError(const std::string &error);
    void eventCallback(DWORD errorCode, DWORD numBytes);
    void handleEvents();
    bool sampleHandled(uint64_t &reads, bool &pendingRename);
    void handleArrival(const std::wstring &fileName);
    // kind: what the entry was, when known, see Event::kind.
    void handleRemoval(const std::wstring &fileName, PathFilter::Kind kind = PathFilter::Kind::UNKNOWN);
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

    bool retryWithNetworkBuffer(DWORD error);
    void allocateWriteBuffer();

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
    const bool mRecursive;
    std::shared_ptr<EventQueue> mQueue;
    HANDLE mDirectoryHandle;
    // Read on the watcher thread, replaced from a worker thread by updateExcludedPaths.
    std::mutex mExcludedPathsMutex;
    std::vector<std::wstring> mExcludedPaths;
    std::shared_ptr<PathFilter> mFilter;

    // The size of the next reads' buffer. Only used on the watcher thread.
    std::size_t mBufferSize;
    std::vector<BYTE> mReadBuffer, mWriteBuffer;
#ifdef NSFW_TEST_FAULTS_1
    // NSFW_TEST_READ_FAULT, read when the watcher is created: network-sync and network-async fail the reads of a
    // buffer larger than a share takes, at once or on completion, as a share does; later-async fails every read after
    // the first on completion, so the failure comes once the watcher has started.
    enum class TestFault { NONE, NETWORK_SYNC, NETWORK_ASYNC, LATER_ASYNC };
    static TestFault readTestFault();
    const TestFault mTestFault = readTestFault();
    unsigned mTestReads = 0;
    bool injectReadFault(DWORD &error);
    static std::atomic<std::size_t> sTestBufferBytes;
    static void countTestBufferBytes(std::size_t added, std::size_t removed) {
      sTestBufferBytes += added;
      sTestBufferBytes -= removed;
    }
#else
    static void countTestBufferBytes(std::size_t, std::size_t) {}
#endif
    // Reads aborted in a row while running. Only used on the watcher thread.
    unsigned mAbortedReads = 0;
    // The old name of a rename whose new name hasn't been read yet, empty if none. Only used on the watcher thread.
    std::wstring mPendingOldName;
    static constexpr ULONGLONG NO_DEADLINE = static_cast<ULONGLONG>(-1);
    ULONGLONG mRenameDeadline = NO_DEADLINE;
    ULONGLONG mRemovalDeadline = NO_DEADLINE;
    void releaseRemoval();
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
