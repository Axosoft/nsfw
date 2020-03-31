#ifndef NSFW_H
#define NSFW_H

#include <atomic>
#include <chrono>
#include <memory>
#include <napi.h>
#include <thread>
#include <vector>

#include "./Queue.h"
#include "./NativeInterface.h"

class NSFW : public Napi::ObjectWrap<NSFW> {
  private:
    static Napi::FunctionReference constructor;
    static std::size_t instanceCount;
    static bool gcEnabled;

    std::unique_ptr<NativeInterface> mInterface;
    Napi::ThreadSafeFunction mEventCallback;
    Napi::ThreadSafeFunction mErrorCallback;
    std::mutex mInterfaceLock;
    std::string mPath;
    std::shared_ptr<EventQueue> mQueue;
    std::thread mPollThread;
    std::atomic<bool> mRunning;
    uint32_t mDebounceMS;

    class StartWorker: public Napi::AsyncWorker {
      public:
        StartWorker(Napi::Env env, NSFW *nsfw);
        void Execute();
        void OnOK();
        Napi::Promise RunJob();

      private:
        Napi::Promise::Deferred mDeferred;
        bool mDidStartWatching;
        NSFW *mNSFW;
        bool mShouldUnref;
    };

    Napi::Value Start(const Napi::CallbackInfo &info);

    class StopWorker: public Napi::AsyncWorker {
      public:
        StopWorker(Napi::Env env, NSFW *nsfw);
        void Execute();
        void OnOK();
        Napi::Promise RunJob();

      private:
        Napi::Promise::Deferred mDeferred;
        bool mDidStopWatching;
        NSFW *mNSFW;
    };

    Napi::Value Stop(const Napi::CallbackInfo &info);

  public:
    static Napi::Object Init(Napi::Env, Napi::Object exports);
    static Napi::Value InstanceCount(const Napi::CallbackInfo &info);
    void pollForEvents();

    NSFW(const Napi::CallbackInfo &info);
    ~NSFW();
};

#endif
