#include "../includes/NSFW.h"

#if defined(_WIN32)
#include <windows.h>
#define sleep_for_ms(ms) Sleep(ms)
#else
#include <unistd.h>
#define sleep_for_ms(ms) usleep(ms * 1000)
#endif

#pragma unmanaged
Persistent<v8::Function> NSFW::constructor;

NSFW::NSFW(uint32_t debounceMS, std::string path, Callback *eventCallback, Callback *errorCallback):
  mDebounceMS(debounceMS),
  mErrorCallback(errorCallback),
  mEventCallback(eventCallback),
  mInterface(NULL),
  mInterfaceLockValid(false),
  mPath(path),
  mRunning(false) {
    HandleScope scope;
    v8::Local<v8::Object> obj = New<v8::Object>();
    mPersistentHandle.Reset(obj);
    mInterfaceLockValid = uv_mutex_init(&mInterfaceLock) == 0;
  }

NSFW::~NSFW() {
  if (mInterface != NULL) {
    delete mInterface;
  }
  delete mEventCallback;
  delete mErrorCallback;

  if (mInterfaceLockValid) {
    uv_mutex_destroy(&mInterfaceLock);
  }
}

void NSFW::cleanupEventCallback(void *arg) {
  EventBaton *baton = (EventBaton *)arg;
  for (uint32_t i = 0; i < baton->events->size(); ++i) {
    delete (*baton->events)[i];
    (*baton->events)[i] = NULL;
  }
  delete baton->events;
  delete baton;
}

void NSFW::fireErrorCallback(uv_async_t *handle) {
  Nan::HandleScope scope;
  ErrorBaton *baton = (ErrorBaton *)handle->data;
  v8::Local<v8::Value> argv[] = {
    New<v8::String>(baton->error).ToLocalChecked()
  };
  baton->nsfw->mErrorCallback->Call(1, argv);
  delete baton;
}

void NSFW::fireEventCallback(uv_async_t *handle) {
  Nan::HandleScope scope;
  EventBaton *baton = (EventBaton *)handle->data;
  if (baton->events->empty()) {
    uv_thread_t cleanup;
    uv_thread_create(&cleanup, NSFW::cleanupEventCallback, baton);
    return;
  }

  std::vector< v8::Local<v8::Object> > *jsEventObjects = new std::vector< v8::Local<v8::Object> >;
  jsEventObjects->reserve(baton->events->size());

  for (auto i = baton->events->begin(); i != baton->events->end(); ++i) {
    v8::Local<v8::Object> anEvent = New<v8::Object>();

    anEvent->Set(New<v8::String>("action").ToLocalChecked(), New<v8::Number>((*i)->type));
    anEvent->Set(New<v8::String>("directory").ToLocalChecked(), New<v8::String>((*i)->directory).ToLocalChecked());

    if ((*i)->type == RENAMED) {
      anEvent->Set(New<v8::String>("oldFile").ToLocalChecked(), New<v8::String>((*i)->fileA).ToLocalChecked());
      anEvent->Set(New<v8::String>("newFile").ToLocalChecked(), New<v8::String>((*i)->fileB).ToLocalChecked());
    } else {
      anEvent->Set(New<v8::String>("file").ToLocalChecked(), New<v8::String>((*i)->fileA).ToLocalChecked());
    }

    jsEventObjects->push_back(anEvent);
  }

  v8::Local<v8::Array> eventArray = New<v8::Array>((int)jsEventObjects->size());

  for (unsigned int i = 0; i < jsEventObjects->size(); ++i) {
    eventArray->Set(i, (*jsEventObjects)[i]);
  }

  v8::Local<v8::Value> argv[] = {
    eventArray
  };

  baton->nsfw->mEventCallback->Call(1, argv);

  delete jsEventObjects;

  uv_thread_t cleanup;
  uv_thread_create(&cleanup, NSFW::cleanupEventCallback, baton);
}

void NSFW::pollForEvents(void *arg) {
  NSFW *nsfw = (NSFW *)arg;
  while(nsfw->mRunning) {
    uv_mutex_lock(&nsfw->mInterfaceLock);

    if (nsfw->mInterface->hasErrored()) {
      ErrorBaton *baton = new ErrorBaton;
      baton->nsfw = nsfw;
      baton->error = nsfw->mInterface->getError();

      nsfw->mErrorCallbackAsync.data = (void *)baton;
      uv_async_send(&nsfw->mErrorCallbackAsync);
      nsfw->mRunning = false;
      uv_mutex_unlock(&nsfw->mInterfaceLock);
      break;
    }
    std::vector<Event *> *events = nsfw->mInterface->getEvents();
    if (events == NULL) {
      uv_mutex_unlock(&nsfw->mInterfaceLock);
      sleep_for_ms(50);
      continue;
    }

    EventBaton *baton = new EventBaton;
    baton->nsfw = nsfw;
    baton->events = events;

    nsfw->mEventCallbackAsync.data = (void *)baton;
    uv_async_send(&nsfw->mEventCallbackAsync);

    uv_mutex_unlock(&nsfw->mInterfaceLock);

    sleep_for_ms(nsfw->mDebounceMS);
  }
}

NAN_MODULE_INIT(NSFW::Init) {
  Nan::HandleScope scope;

  v8::Local<v8::FunctionTemplate> tpl = New<v8::FunctionTemplate>(JSNew);
  tpl->SetClassName(New<v8::String>("NSFW").ToLocalChecked());
  tpl->InstanceTemplate()->SetInternalFieldCount(1);

  SetPrototypeMethod(tpl, "start", Start);
  SetPrototypeMethod(tpl, "stop", Stop);

  constructor.Reset(tpl->GetFunction());
  Set(target, New<v8::String>("NSFW").ToLocalChecked(), tpl->GetFunction());
}

NAN_METHOD(NSFW::JSNew) {
  if (!info.IsConstructCall()) {
    v8::Local<v8::Function> cons = New<v8::Function>(constructor);
    info.GetReturnValue().Set(cons->NewInstance());
    return;
  }

  if (info.Length() < 1 || !info[0]->IsUint32()) {
    return ThrowError("First argument of constructor must be a positive integer.");
  }
  if (info.Length() < 2 || !info[1]->IsString()) {
    return ThrowError("Second argument of constructor must be a path.");
  }
  if (info.Length() < 3 || !info[2]->IsFunction()) {
    return ThrowError("Third argument of constructor must be a callback.");
  }
  if (info.Length() < 4 || !info[3]->IsFunction()) {
    return ThrowError("Fourth argument of constructor must be a callback.");
  }

  uint32_t debounceMS = info[0]->Uint32Value();
  v8::String::Utf8Value utf8Value(info[1]->ToString());
  std::string path = std::string(*utf8Value);
  Callback *eventCallback = new Callback(info[2].As<v8::Function>());
  Callback *errorCallback = new Callback(info[3].As<v8::Function>());

  NSFW *nsfw = new NSFW(debounceMS, path, eventCallback, errorCallback);
  nsfw->Wrap(info.This());
  info.GetReturnValue().Set(info.This());
}

NAN_METHOD(NSFW::Start) {
  Nan::HandleScope scope;

  NSFW *nsfw = ObjectWrap::Unwrap<NSFW>(info.This());
  if (!nsfw->mInterfaceLockValid) {
    return ThrowError("NSFW failed to initialize properly. Try creating a new NSFW.");
  }

  if (
    info.Length() < 1 ||
    !info[0]->IsFunction()
  ) {
    return ThrowError("Must provide callback to start.");
  }

  Callback *callback = new Callback(info[0].As<v8::Function>());

  if (nsfw->mInterface != NULL) {
    v8::Local<v8::Value> argv[1] = {
      Nan::Error("This NSFW cannot be started, because it is already running.")
    };
    callback->Call(1, argv);
    delete callback;
    return;
  }

  New(nsfw->mPersistentHandle)->Set(New("nsfw").ToLocalChecked(), info.This());

  AsyncQueueWorker(new StartWorker(nsfw, callback));
}

NSFW::StartWorker::StartWorker(NSFW *nsfw, Callback *callback):
  AsyncWorker(callback), mNSFW(nsfw) {
    uv_async_init(uv_default_loop(), &nsfw->mErrorCallbackAsync, &NSFW::fireErrorCallback);
    uv_async_init(uv_default_loop(), &nsfw->mEventCallbackAsync, &NSFW::fireEventCallback);
  }

void NSFW::StartWorker::Execute() {
  uv_mutex_lock(&mNSFW->mInterfaceLock);

  if (mNSFW->mInterface != NULL) {
    uv_mutex_unlock(&mNSFW->mInterfaceLock);
    return;
  }

  mNSFW->mInterface = new NativeInterface(mNSFW->mPath);
  if (mNSFW->mInterface->isWatching()) {
    mNSFW->mRunning = true;
    uv_thread_create(&mNSFW->mPollThread, NSFW::pollForEvents, mNSFW);
  } else {
    delete mNSFW->mInterface;
    mNSFW->mInterface = NULL;
  }

  uv_mutex_unlock(&mNSFW->mInterfaceLock);
}

void NSFW::StartWorker::HandleOKCallback() {
  HandleScope();
  if (mNSFW->mInterface == NULL) {
    if (!mNSFW->mPersistentHandle.IsEmpty()) {
      v8::Local<v8::Object> obj = New<v8::Object>();
      mNSFW->mPersistentHandle.Reset(obj);
    }
    v8::Local<v8::Value> argv[1] = {
      Nan::Error("NSFW was unable to start watching that directory.")
    };
    callback->Call(1, argv);
  } else {
    callback->Call(0, NULL);
  }
}

NAN_METHOD(NSFW::Stop) {
  Nan::HandleScope scope;

  NSFW *nsfw = ObjectWrap::Unwrap<NSFW>(info.This());
  if (!nsfw->mInterfaceLockValid) {
    return ThrowError("NSFW failed to initialize properly. Try creating a new NSFW.");
  }

  if (
    info.Length() < 1 ||
    !info[0]->IsFunction()
  ) {
    return ThrowError("Must provide callback to stop.");
  }

  Callback *callback = new Callback(info[0].As<v8::Function>());

  if (nsfw->mInterface == NULL) {
    v8::Local<v8::Value> argv[1] = {
      Nan::Error("This NSFW cannot be stopped, because it is not running.")
    };
    callback->Call(1, argv);
    delete callback;
    return;
  }

  AsyncQueueWorker(new StopWorker(nsfw, callback));
}

NSFW::StopWorker::StopWorker(NSFW *nsfw, Callback *callback):
  AsyncWorker(callback), mNSFW(nsfw) {}

void NSFW::StopWorker::Execute() {
  uv_mutex_lock(&mNSFW->mInterfaceLock);

  if (mNSFW->mInterface == NULL) {
    uv_mutex_unlock(&mNSFW->mInterfaceLock);
    return;
  }

  mNSFW->mRunning = false;

  uv_thread_join(&mNSFW->mPollThread);

  delete mNSFW->mInterface;
  mNSFW->mInterface = NULL;

  uv_mutex_unlock(&mNSFW->mInterfaceLock);
}

void NSFW::StopWorker::HandleOKCallback() {
  HandleScope();

  if (!mNSFW->mPersistentHandle.IsEmpty()) {
    v8::Local<v8::Object> obj = New<v8::Object>();
    mNSFW->mPersistentHandle.Reset(obj);
  }

  uv_close(reinterpret_cast<uv_handle_t*>(&mNSFW->mErrorCallbackAsync), nullptr);
  uv_close(reinterpret_cast<uv_handle_t*>(&mNSFW->mEventCallbackAsync), nullptr);

  callback->Call(0, NULL);
}

NODE_MODULE(nsfw, NSFW::Init)
