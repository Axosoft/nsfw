#include "../includes/Semaphore.h"

SingleshotSemaphore::SingleshotSemaphore() : mState(false) {}

void Semaphore::wait()
{
    nsfw_sync::Guard lk(mMutex);

    while (!mState) {
        wait(mMutex);
    }
}

void Semaphore::signal()
{
    std::unique_lock<std::mutex> lk(mMutex);
    mState = true;
    mCond.notify_all();
}

void Semaphore::wait(nsfw_sync::Mutex &mutex)
{
    mCond.wait(mutex);
}
