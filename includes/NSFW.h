#ifndef NSFW_H
#define NSFW_H

// node-addon-api 8 needs C++17. binding.gyp pins gnu++17 on macOS; Linux and
// Windows take the standard of the Node or Electron headers, so fail here with a
// clear message instead of deep inside napi.h if those ever set an older one.
// MSVC only reports the real value in __cplusplus with /Zc:__cplusplus, so read
// _MSVC_LANG there.
#if defined(_MSVC_LANG)
#if _MSVC_LANG < 201703L
#error "nsfw requires C++17 or newer"
#endif
#elif __cplusplus < 201703L
#error "nsfw requires C++17 or newer"
#endif

#include <atomic>
#include <deque>
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
    uint64_t mNextRunId = 0;
    uint64_t mRunId = 0;
    bool stopRun(uint64_t runId = 0);

    class Job;
    // Main-thread only; at most the front job is submitted to the thread pool.
    std::deque<Job *> mJobs;
    bool mClosing = false;

    // Base of every async job. It keeps the watcher alive until the job is destroyed, whichever way it ends (OnOK,
    // OnError or cancellation), and rejects the job's promise if Execute throws a std::exception.
    class Job: public Napi::AsyncWorker {
      public:
        Napi::Promise RunJob();
        void OnWorkComplete(Napi::Env env, napi_status status) override;

      protected:
        Job(Napi::Env env, NSFW *nsfw);
        void OnError(const Napi::Error &error) override;

        Napi::Promise::Deferred mDeferred;
        NSFW *mNSFW;

      private:
        Napi::ObjectReference mWatcher;
    };

    class StartWorker: public Job {
      public:
        StartWorker(Napi::Env env, NSFW *nsfw);
        ~StartWorker();
        void Execute();
        void OnOK();

      private:
        enum JobStatus { STARTED, ALREADY_RUNNING, COULD_NOT_START, JOB_NOT_EXECUTED_YET };
        Napi::ThreadSafeFunction mErrorCallback;
        Napi::ThreadSafeFunction mEventCallback;
        JobStatus mStatus;
        uint64_t mRunId;
    };

    Napi::Value Start(const Napi::CallbackInfo &info);

    class StopWorker: public Job {
      public:
        StopWorker(Napi::Env env, NSFW *nsfw);
        void Execute();
        void OnOK();

      private:
        bool mDidStopWatching;
    };

    Napi::Value Stop(const Napi::CallbackInfo &info);

    class PauseWorker: public Job {
      public:
        PauseWorker(Napi::Env env, NSFW *nsfw);
        void Execute();
        void OnOK();

      private:
        std::atomic<bool> mDidPauseEvents;
    };

    Napi::Value Pause(const Napi::CallbackInfo &info);

    class ResumeWorker: public Job {
      public:
        ResumeWorker(Napi::Env env, NSFW *nsfw);
        void Execute();
        void OnOK();

      private:
        std::atomic<bool> mDidResumeEvents;
    };

    Napi::Value Resume(const Napi::CallbackInfo &info);

    class GetExcludedPathsWorker: public Job {
      public:
        GetExcludedPathsWorker(Napi::Env env, NSFW *nsfw);
        void Execute();
        void OnOK();

      private:
        std::atomic<bool> mDidGetExcludedPaths;
    };

    Napi::Value GetExcludedPaths(const Napi::CallbackInfo &info);

    class UpdateExcludedPathsWorker: public Job {
      public:
        UpdateExcludedPathsWorker(Napi::Env env, const Napi::CallbackInfo &info, NSFW *nsfw);
        void Execute();
        void OnOK();

      private:
        std::atomic<bool> mDidUpdatetExcludedPaths;
        // The normalized list this call set, which its promise resolves with.
        std::vector<std::string> mExcludedPaths;
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
