#include "../includes/NSFW.h"

#include <chrono>
#include <condition_variable>
#include <exception>
#include <thread>

// One run of the watcher, from a successful start() until stop(), a watcher error or teardown. It owns the native interface, the
// poll thread and the thread-safe functions that thread calls, so a run is created and destroyed as one unit. The
// poll thread releases the thread-safe functions when it ends, whether stopped or after a watcher error, and only
// then do they stop keeping the event loop alive.
class NSFW::Run {
  public:
    Run(
      std::unique_ptr<NativeInterface> nativeInterface,
      std::shared_ptr<EventQueue> queue,
      uint32_t debounceMS,
      Napi::ThreadSafeFunction eventCallback,
      Napi::ThreadSafeFunction errorCallback,
      NSFW *watcher,
      uint64_t runId
    ):
      mInterface(std::move(nativeInterface)),
      mQueue(queue),
      mDebounceMS(debounceMS),
      mEventCallback(eventCallback),
      mErrorCallback(errorCallback),
      mStopping(false),
      mWatcher(watcher),
      mRunId(runId)
    {
      // Last, so that a failure leaves the thread-safe functions unreleased for the caller to release.
      mThread = std::thread(&Run::poll, this);
    }

    ~Run() {
      {
        std::lock_guard<std::mutex> lock(mLock);
        mStopping = true;
      }
      mWake.notify_one();
      mThread.join();
    }

    void updateExcludedPaths(const std::vector<std::string> &excludedPaths) {
      std::lock_guard<std::mutex> lock(mLock);
      mInterface->updateExcludedPaths(excludedPaths);
    }

  private:
    void poll();

    std::unique_ptr<NativeInterface> mInterface;
    std::shared_ptr<EventQueue> mQueue;
    uint32_t mDebounceMS;
    Napi::ThreadSafeFunction mEventCallback;
    Napi::ThreadSafeFunction mErrorCallback;
    // Guards mInterface and mStopping.
    std::mutex mLock;
    std::condition_variable mWake;
    bool mStopping;
    std::thread mThread;
    NSFW *mWatcher;
    uint64_t mRunId;
};

void NSFW::Run::poll() {
  std::unique_lock<std::mutex> lock(mLock);
  while (!mStopping) {
    uint32_t sleepDuration = 50;

    if (mInterface->hasErrored()) {
      const std::string &error = mInterface->getError();
      NSFW *watcher = mWatcher;
      uint64_t runId = mRunId;
      mErrorCallback.NonBlockingCall([error, watcher, runId](Napi::Env env, Napi::Function jsCallback) {
        if (!watcher->stopRun(runId)) return;
        Napi::Value jsError = Napi::Error::New(env, error).Value();
        jsCallback.Call({ jsError });
      });
      break;
    }

    if (mQueue->count() != 0) {
      auto events = mQueue->dequeueAll();
      if (events != nullptr) {
        sleepDuration = mDebounceMS;
        auto callback = [](Napi::Env env, Napi::Function jsCallback, std::vector<std::unique_ptr<Event>> *eventsRaw) {
          std::unique_ptr<std::vector<std::unique_ptr<Event>>> events(eventsRaw);
          eventsRaw = nullptr;

          int numEvents = events->size();
          Napi::Array eventArray = Napi::Array::New(env, numEvents);

          for (int i = 0; i < numEvents; ++i) {
            auto event = Napi::Object::New(env);
            event["action"] = Napi::Number::New(env, (*events)[i]->type);
            event["directory"] = Napi::String::New(env, (*events)[i]->fromDirectory);

            if ((*events)[i]->type == RENAMED) {
              event["oldFile"] = Napi::String::New(env, (*events)[i]->fromFile);
              event["newDirectory"] = Napi::String::New(env, (*events)[i]->toDirectory);
              event["newFile"] = Napi::String::New(env, (*events)[i]->toFile);
            } else {
              event["file"] = Napi::String::New(env, (*events)[i]->fromFile);
            }

            eventArray[(uint32_t)i] = event;
          }

          jsCallback.Call({ eventArray });
        };

        mEventCallback.NonBlockingCall(events.release(), callback);
      }
    }

    mWake.wait_for(lock, std::chrono::milliseconds(sleepDuration), [this]() { return mStopping; });
  }
  lock.unlock();

  // Once released, and their queued calls delivered, the callbacks stop keeping the event loop alive.
  mErrorCallback.Release();
  mEventCallback.Release();
}

const char *const NSFW::EVENT_CALLBACK_KEY = "__nsfwEventCallback";
const char *const NSFW::ERROR_CALLBACK_KEY = "__nsfwErrorCallback";

std::size_t NSFW::instanceCount = 0;
bool NSFW::gcEnabled = false;

NSFW::NSFW(const Napi::CallbackInfo &info):
  Napi::ObjectWrap<NSFW>(info),
  mDebounceMS(0),
  mQueue(std::make_shared<EventQueue>()),
  mPath("")
{
  Napi::Env env = info.Env();

  if (info.Length() < 1 || !info[0].IsString()) {
    throw Napi::TypeError::New(env, "Must pass a string path as the first argument to NSFW.");
  }

  mPath = info[0].ToString();

  if (info.Length() < 2 || !info[1].IsFunction()) {
    throw Napi::TypeError::New(env, "Must pass an event callback as the second parameter to NSFW.");
  }

  Napi::Function errorCallback = Napi::Function::New(env, [](const Napi::CallbackInfo &info) {});

  if (info.Length() >= 3) {
    if (!info[2].IsObject()) {
      throw Napi::TypeError::New(env, "If the third parameter to NSFW is provided, it must be an object.");
    }

    Napi::Object options = info[2].ToObject();

    // debounceMS
    Napi::Value maybeDebounceMS = options["debounceMS"];
    if (options.Has("debounceMS") && !maybeDebounceMS.IsNumber()) {
      throw Napi::TypeError::New(env, "options.debounceMS must be a number.");
    }

    if (maybeDebounceMS.IsNumber()) {
      Napi::Number temp = maybeDebounceMS.ToNumber();
      double bounds = temp.DoubleValue();
      if (bounds < 1 || bounds > 60000) {
        throw Napi::TypeError::New(env, "options.debounceMS must be >= 1 and <= 60000.");
      }

      mDebounceMS = temp;
    }

    // errorCallback
    Napi::Value maybeErrorCallback = options["errorCallback"];
    if (options.Has("errorCallback") && !maybeErrorCallback.IsFunction()) {
      throw Napi::TypeError::New(env, "options.errorCallback must be a function.");
    }

    if (maybeErrorCallback.IsFunction()) {
      errorCallback = maybeErrorCallback.As<Napi::Function>();
    }

    // excludedPaths
    Napi::Value maybeExcludedPaths = options["excludedPaths"];
    if (options.Has("excludedPaths") && !maybeExcludedPaths.IsArray()) {
      throw Napi::TypeError::New(env, "options.excludedPaths must be an array.");
    }
    if (maybeExcludedPaths.IsArray()) {
      Napi::Array paths = maybeExcludedPaths.As<Napi::Array>();
      for(uint32_t i = 0; i < paths.Length(); i++) {
        Napi::Value path = paths[i];
        if (path.IsString())
        {
          std::string str = path.ToString().Utf8Value();
          if (!str.empty() && str.back() == '/') {
            str.pop_back();
          }
          mExcludedPaths.push_back(str);
        } else {
          throw Napi::TypeError::New(env, "options.excludedPaths elements must be strings.");
        }
      }
    }

  }

  // Between runs the callbacks are plain, non-enumerable properties of this object; each start() wraps them in
  // thread-safe functions owned by that run. Native references would root a callback that captures its own watcher
  // and keep the watcher alive forever; as properties they form a JS cycle the garbage collector reclaims.
  info.This().As<Napi::Object>().DefineProperties({
    Napi::PropertyDescriptor::Value(EVENT_CALLBACK_KEY, info[1], napi_default),
    Napi::PropertyDescriptor::Value(ERROR_CALLBACK_KEY, errorCallback, napi_default)
  });

  napi_add_env_cleanup_hook(env, NSFW::onEnvCleanup, this);

  if (gcEnabled) {
    instanceCount++;
  }
}

// On environment teardown (worker.terminate(), process exit) the thread-safe functions' own cleanup hooks only close
// their handles; Node frees them in the close callbacks, after every cleanup hook has run. Ending the run here, in the
// same pass, means its poll thread never touches a freed one. This object is only finalized after that.
void NSFW::onEnvCleanup(void *data) {
  NSFW *watcher = static_cast<NSFW *>(data);
  watcher->mClosing = true;
  while (watcher->mJobs.size() > 1) {
    Job *job = watcher->mJobs.back();
    watcher->mJobs.pop_back();
    delete job;
  }
  watcher->stopRun();
}

// Ends the run under mRunLock, so a start() cannot begin while the old run's interface may still queue events. Of
// several concurrent callers only the first finds a run to end.
bool NSFW::stopRun(uint64_t runId) {
  std::lock_guard<std::mutex> lock(mRunLock);
  if (!mRun || (runId != 0 && runId != mRunId)) {
    return false;
  }
  mRun.reset();
  mQueue->clear();
  return true;
}

NSFW::~NSFW() {
  // A hook left behind would run on a destroyed object when the environment exits.
  napi_remove_env_cleanup_hook(Env(), NSFW::onEnvCleanup, this);
  stopRun();
  if (gcEnabled) {
    instanceCount--;
  }
}

NSFW::Job::Job(Napi::Env env, NSFW *nsfw):
  Napi::AsyncWorker(env, "nsfw"),
  mDeferred(Napi::Promise::Deferred::New(env)),
  mNSFW(nsfw),
  mWatcher(Napi::Persistent(nsfw->Value()))
{}

Napi::Promise NSFW::Job::RunJob() {
  mNSFW->mJobs.push_back(this);
  if (mNSFW->mJobs.size() == 1) {
    this->Queue();
  }
  return mDeferred.Promise();
}

void NSFW::Job::OnWorkComplete(Napi::Env env, napi_status status) {
  // Queue only the next job, so call order never depends on thread-pool scheduling.
  mNSFW->mJobs.pop_front();
  if (!mNSFW->mClosing && !mNSFW->mJobs.empty()) {
    mNSFW->mJobs.front()->Queue();
  }
  Napi::AsyncWorker::OnWorkComplete(env, status);
}

void NSFW::Job::OnError(const Napi::Error &error) {
  mDeferred.Reject(error.Value());
}

NSFW::StartWorker::StartWorker(Napi::Env env, NSFW *nsfw):
  Job(env, nsfw),
  mStatus(JOB_NOT_EXECUTED_YET),
  mRunId(++nsfw->mNextRunId)
{
  // One pair per run, created here because it needs the main thread. They keep the event loop alive until the
  // poll thread releases them when the run ends.
  Napi::Object self = nsfw->Value();
  mEventCallback = Napi::ThreadSafeFunction::New(env, self.Get(EVENT_CALLBACK_KEY).As<Napi::Function>(), "nsfw", 0, 1);
  try {
    uint64_t runId = mRunId;
    mErrorCallback = Napi::ThreadSafeFunction::New(
      env, self.Get(ERROR_CALLBACK_KEY).As<Napi::Function>(), "nsfw", 0, 1,
      [nsfw, runId](Napi::Env) {
        nsfw->stopRun(runId);
        nsfw->Unref();
      }
    );
  } catch (...) {
    mEventCallback.Release();
    throw;
  }

  // Queued error delivery and its finalizer can outlive stop(). Keep the watcher alive until both have finished.
  mNSFW->Ref();
}

NSFW::StartWorker::~StartWorker() {
  // Not handed to a run: the watcher did not start.
  if (mStatus != STARTED) {
    mErrorCallback.Release();
    mEventCallback.Release();
  }
}

void NSFW::StartWorker::Execute() {
  std::lock_guard<std::mutex> lock(mNSFW->mRunLock);

  if (mNSFW->mRun) {
    mStatus = ALREADY_RUNNING;
    return;
  }

  std::vector<std::string> excludedPaths;
  {
    std::lock_guard<std::mutex> excludedPathsLock(mNSFW->mExcludedPathsLock);
    excludedPaths = mNSFW->mExcludedPaths;
  }

  try {
    mNSFW->mQueue->clear();
    std::unique_ptr<NativeInterface> nativeInterface(
      new NativeInterface(mNSFW->mPath, excludedPaths, mNSFW->mQueue)
    );
    if (!nativeInterface->isWatching()) {
      mStatus = COULD_NOT_START;
      return;
    }

    mNSFW->mRun.reset(new Run(
      std::move(nativeInterface),
      mNSFW->mQueue,
      mNSFW->mDebounceMS,
      mEventCallback,
      mErrorCallback,
      mNSFW,
      mRunId
    ));
    mNSFW->mRunId = mRunId;
  } catch (const std::exception &) {
    mStatus = COULD_NOT_START;
    return;
  }
  // Only now does the run own this start()'s callbacks; until here ~StartWorker releases them.
  mStatus = STARTED;
}

void NSFW::StartWorker::OnOK() {
  auto env = Env();
  switch (mStatus) {
    case ALREADY_RUNNING:
      mDeferred.Reject(Napi::Error::New(env, "This NSFW cannot be started, because it is already running.").Value());
      break;

    case COULD_NOT_START:
      mDeferred.Reject(Napi::Error::New(env, "NSFW was unable to start watching that directory.").Value());
      break;

    case STARTED:
      mDeferred.Resolve(env.Undefined());
      break;

    default:
      mDeferred.Reject(Napi::Error::New(
        env,
        "Execute did not run, but OnOK fired. This should never have happened."
      ).Value());
  }
}

Napi::Value NSFW::Start(const Napi::CallbackInfo &info) {
  return (new StartWorker(info.Env(), this))->RunJob();
}

NSFW::StopWorker::StopWorker(Napi::Env env, NSFW *nsfw):
  Job(env, nsfw),
  mDidStopWatching(false)
{}

void NSFW::StopWorker::Execute() {
  // Of several concurrent stop() calls only the one that takes the run stops the watcher; the others reject.
  mDidStopWatching = mNSFW->stopRun();
}

void NSFW::StopWorker::OnOK() {
  if (mDidStopWatching) {
    mDeferred.Resolve(Env().Undefined());
  } else {
    mDeferred.Reject(Napi::Error::New(Env(), "This NSFW cannot be stopped, because it is not running.").Value());
  }
}

Napi::Value NSFW::Stop(const Napi::CallbackInfo &info) {
  return (new StopWorker(info.Env(), this))->RunJob();
}

NSFW::PauseWorker::PauseWorker(Napi::Env env, NSFW *nsfw):
  Job(env, nsfw),
  mDidPauseEvents(false)
{}

void NSFW::PauseWorker::Execute() {
  mDidPauseEvents = true;
  mNSFW->pauseQueue();
}

void NSFW::PauseWorker::OnOK() {
  if (mDidPauseEvents) {
    mDeferred.Resolve(Env().Undefined());
  } else {
    mDeferred.Reject(Napi::Error::New(Env(), "This NSFW could not be paused.").Value());
  }
}

Napi::Value NSFW::Pause(const Napi::CallbackInfo &info) {
  return (new PauseWorker(info.Env(), this))->RunJob();
}

NSFW::ResumeWorker::ResumeWorker(Napi::Env env, NSFW *nsfw):
  Job(env, nsfw),
  mDidResumeEvents(false)
{}

void NSFW::ResumeWorker::Execute() {
  mDidResumeEvents = true;
  mNSFW->resumeQueue();
}

void NSFW::ResumeWorker::OnOK() {
  if (mDidResumeEvents) {
    mDeferred.Resolve(Env().Undefined());
  } else {
    mDeferred.Reject(Napi::Error::New(Env(), "This NSFW could not be resumed.").Value());
  }
}

Napi::Value NSFW::Resume(const Napi::CallbackInfo &info) {
  return (new ResumeWorker(info.Env(), this))->RunJob();
}

NSFW::GetExcludedPathsWorker::GetExcludedPathsWorker(Napi::Env env, NSFW *nsfw):
  Job(env, nsfw),
  mDidGetExcludedPaths(false)
{}

void NSFW::GetExcludedPathsWorker::Execute() {
  mDidGetExcludedPaths = true;
}

void NSFW::GetExcludedPathsWorker::OnOK() {
  if (mDidGetExcludedPaths) {
    mDeferred.Resolve(mNSFW->ExcludedPaths());
  } else {
    mDeferred.Reject(Napi::Error::New(Env(), "Excluded Paths cannot be obtained.").Value());
  }
}

Napi::Value NSFW::GetExcludedPaths(const Napi::CallbackInfo &info) {
  return (new GetExcludedPathsWorker(info.Env(), this))->RunJob();
}

NSFW::UpdateExcludedPathsWorker::UpdateExcludedPathsWorker(Napi::Env env, const Napi::CallbackInfo &info, NSFW *nsfw):
  Job(info.Env(), nsfw),
  mDidUpdatetExcludedPaths(false)
{
  if (info.Length() < 1 || !info[0].IsArray()) {
    throw Napi::TypeError::New(env, "Must pass an array of string to updateExcludedPaths.");
  }

  // excludedPaths
  Napi::Value maybeExcludedPaths = info[0];
  Napi::Array paths = maybeExcludedPaths.As<Napi::Array>();
  for(uint32_t i = 0; i < paths.Length(); i++) {
    if (!((Napi::Value)paths[i]).IsString()) {
      throw Napi::TypeError::New(env, "excludedPaths elements must be strings.");
    }
  }
  std::vector<std::string> excludedPaths;
  for(uint32_t i = 0; i < paths.Length(); i++) {
    Napi::Value path = paths[i];
    std::string str = path.ToString().Utf8Value();
    if (!str.empty() && str.back() == '/') {
      str.pop_back();
    }
    excludedPaths.push_back(str);
  }
  // Stored here, on the main thread, so updates apply in call order; Execute only forwards them to a running watcher.
  std::lock_guard<std::mutex> lock(mNSFW->mExcludedPathsLock);
  mNSFW->mExcludedPaths = excludedPaths;
}

void NSFW::UpdateExcludedPathsWorker::Execute() {
  std::lock_guard<std::mutex> lock(mNSFW->mRunLock);
  // A stopped watcher only keeps them for its next start().
  if (mNSFW->mRun) {
    std::vector<std::string> excludedPaths;
    {
      std::lock_guard<std::mutex> excludedPathsLock(mNSFW->mExcludedPathsLock);
      excludedPaths = mNSFW->mExcludedPaths;
    }
    mNSFW->mRun->updateExcludedPaths(excludedPaths);
  }
  mDidUpdatetExcludedPaths = true;
}

void NSFW::UpdateExcludedPathsWorker::OnOK() {
  if (mDidUpdatetExcludedPaths) {
    mDeferred.Resolve(mNSFW->ExcludedPaths());
  } else {
    mDeferred.Reject(Napi::Error::New(Env(), "Excluded Paths cannot be obtained.").Value());
  }
}

Napi::Value NSFW::UpdateExcludedPaths(const Napi::CallbackInfo &info) {
  return (new UpdateExcludedPathsWorker(info.Env(), info, this))->RunJob();
}

void NSFW::pauseQueue() {
  mQueue->pause();
}

void NSFW::resumeQueue() {
  mQueue->resume();
}

Napi::Value NSFW::ExcludedPaths() {
  std::lock_guard<std::mutex> lock(mExcludedPathsLock);
  Napi::Array path_array = Napi::Array::New(Env(), mExcludedPaths.size());

  uint32_t i = 0;
  for (auto&& path : mExcludedPaths) {
    path_array[i++] = Napi::String::New(Env(), path);
  }

  return path_array;
}

Napi::Value NSFW::InstanceCount(const Napi::CallbackInfo &info) {
  return Napi::Number::New(info.Env(), instanceCount);
}

Napi::Object NSFW::Init(Napi::Env env, Napi::Object exports) {
  gcEnabled = ((Napi::Value)env.Global()["gc"]).IsFunction();

  Napi::Function nsfwConstructor = DefineClass(env, "NSFW", {
    InstanceMethod("start", &NSFW::Start),
    InstanceMethod("stop", &NSFW::Stop),
    InstanceMethod("pause", &NSFW::Pause),
    InstanceMethod("resume", &NSFW::Resume),
    InstanceMethod("getExcludedPaths", &NSFW::GetExcludedPaths),
    InstanceMethod("updateExcludedPaths", &NSFW::UpdateExcludedPaths)
  });

  if (gcEnabled) {
    nsfwConstructor.DefineProperty(Napi::PropertyDescriptor::Function(
      "getAllocatedInstanceCount",
      &NSFW::InstanceCount,
      napi_static
    ));
  }

  #ifdef NSFW_TEST_SLOW_1
    nsfwConstructor.DefineProperty(Napi::PropertyDescriptor::Value(
      "NSFW_TEST_SLOW",
      Napi::Boolean::New(env, true)
    ));
  #endif

  return nsfwConstructor;
}

static Napi::Object Init(Napi::Env env, Napi::Object exports) {
  return NSFW::Init(env, exports);
}

NODE_API_MODULE(nsfw, Init)
