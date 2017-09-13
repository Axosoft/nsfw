#ifndef NSFW_SEMAPHORE_H
#define NSFW_SEMAPHORE_H

#include <condition_variable>
#include <mutex>

class SingleshotSemaphore {
  public:
    SingleshotSemaphore() : mState(false) {}

    void wait()
    {
        std::unique_lock<std::mutex> lk(mMutex);

        while (!mState) {
            mCond.wait(lk);
        }
    }

    void signal()
    {
        std::unique_lock<std::mutex> lk(mMutex);
        mState = true;
        mCond.notify_all();
    }

  private:
    std::mutex              mMutex;
    std::condition_variable mCond;
    bool                    mState;
};

#endif
