#ifndef NSFW_H
#define NSFW_H

#include <atomic>
#include <memory>
#include <mutex>
#include <napi.h>
#include <vector>

#include "./Queue.h"
#include "./NativeInterface.h"

class NSFW : public Napi::ObjectWrap<NSFW> {
  private:
    Napi::Value ExcludedPaths();
    static std::size_t instanceCount;
    static bool gcEnabled;

    // Everything that exists only while the watcher runs, see NSFW.cpp.
    class Run;

    static const char *const EVENT_CALLBACK_KEY;
    static const char *const ERROR_CALLBACK_KEY;

    uint32_t mDebounceMS;
    std::shared_ptr<EventQueue> mQueue;
    std::string mPath;
    // Guards mRun, including while a run is being created or destroyed.
    std::mutex mRunLock;
    std::unique_ptr<Run> mRun;
    // Only ever held briefly, so the main thread can take it.
    std::mutex mExcludedPathsLock;
    std::vector<std::string> mExcludedPaths;
    bool stopRun();

    class StartWorker: public Napi::AsyncWorker {
      public:
        StartWorker(Napi::Env env, NSFW *nsfw);
        ~StartWorker();
        void Execute();
        void OnOK();
        Napi::Promise RunJob();

      private:
        enum JobStatus { STARTED, ALREADY_RUNNING, COULD_NOT_START, JOB_NOT_EXECUTED_YET };
        Napi::Promise::Deferred mDeferred;
        Napi::ThreadSafeFunction mErrorCallback;
        Napi::ThreadSafeFunction mEventCallback;
        NSFW *mNSFW;
        JobStatus mStatus;
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

    class PauseWorker: public Napi::AsyncWorker {
      public:
        PauseWorker(Napi::Env env, NSFW *nsfw);
        void Execute();
        void OnOK();
        Napi::Promise RunJob();

      private:
        Napi::Promise::Deferred mDeferred;
        std::atomic<bool> mDidPauseEvents;
        NSFW *mNSFW;
    };

    Napi::Value Pause(const Napi::CallbackInfo &info);

    class ResumeWorker: public Napi::AsyncWorker {
      public:
        ResumeWorker(Napi::Env env, NSFW *nsfw);
        void Execute();
        void OnOK();
        Napi::Promise RunJob();

      private:
        Napi::Promise::Deferred mDeferred;
        std::atomic<bool> mDidResumeEvents;
        NSFW *mNSFW;
    };

    Napi::Value Resume(const Napi::CallbackInfo &info);

    class GetExcludedPathsWorker: public Napi::AsyncWorker {
      public:
        GetExcludedPathsWorker(Napi::Env env, NSFW *nsfw);
        void Execute();
        void OnOK();
        Napi::Promise RunJob();

      private:
        Napi::Promise::Deferred mDeferred;
        std::atomic<bool> mDidGetExcludedPaths;
        NSFW *mNSFW;
    };

    Napi::Value GetExcludedPaths(const Napi::CallbackInfo &info);

    class UpdateExcludedPathsWorker: public Napi::AsyncWorker {
      public:
        UpdateExcludedPathsWorker(Napi::Env env, const Napi::CallbackInfo &info, NSFW *nsfw);
        void Execute();
        void OnOK();
        Napi::Promise RunJob();

      private:
        Napi::Promise::Deferred mDeferred;
        std::atomic<bool> mDidUpdatetExcludedPaths;
        NSFW *mNSFW;
    };

    Napi::Value UpdateExcludedPaths(const Napi::CallbackInfo &info);

  public:
    static Napi::Object Init(Napi::Env, Napi::Object exports);
    static Napi::Value InstanceCount(const Napi::CallbackInfo &info);
    static void onEnvCleanup(void *data);
    void pauseQueue();
    void resumeQueue();

    NSFW(const Napi::CallbackInfo &info);
    ~NSFW();
};

#endif
