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

    QueueUserAPC([](__in ULONG_PTR self) {
        auto watcher = reinterpret_cast<Watcher*>(self);
        watcher->mHasStartedSemaphore.signal();

    } , mRunner.native_handle(), (ULONG_PTR)this);
    mHasStartedSemaphore.wait();
}

void Watcher::stop()
{
    mRunning = false;
    QueueUserAPC([](__in ULONG_PTR) {}, mRunner.native_handle(), (ULONG_PTR)this);
    mRunner.join();
}
