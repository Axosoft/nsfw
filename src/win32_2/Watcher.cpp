#include "../includes/win32_2/Watcher.h"

Watcher::Watcher(EventQueue &queue, HANDLE dirHandle, const std::wstring &path)
    : mRunning(false)
{
    start();
}

Watcher::~Watcher()
{
    stop();
}

void Watcher::run()
{
    while(mRunning) {
        SleepEx(INFINITE, true);
    }
}

void Watcher::start()
{
    mRunner = std::thread([this]{
        // mRunning is set to false in the d'tor
        mRunning = true;
        run();
    });

    if (!mRunner.joinable()) {
        mRunning = false;
        return;
    }

    QueueUserAPC([](__in ULONG_PTR self) {
        auto watcher = reinterpret_cast<Watcher*>(self);
        watcher->mHasStartedSemaphore.signal();
    } , mRunner.native_handle(), (ULONG_PTR)this);

    if (!mHasStartedSemaphore.waitFor(std::chrono::seconds(10))) {
        setError("Watcher is not started");
    }
}

void Watcher::stop()
{
    mRunning = false;
    QueueUserAPC([](__in ULONG_PTR) {}, mRunner.native_handle(), (ULONG_PTR)this);
    mRunner.join();
}

void Watcher::setError(const std::string &error)
{
    std::lock_guard<std::mutex> lock(mErrorMutex);
    mError = error;
}


std::string Watcher::getError() const
{
    if (!isRunning()) {
        return "Failed to start watcher";
    }

    std::lock_guard<std::mutex> lock(mErrorMutex);
    return mError;
}
