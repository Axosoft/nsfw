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
  mRunning(false),
  mQueue(std::make_shared<EventQueue>())
  {
    HandleScope scope;
    v8::Local<v8::Object> obj = New<v8::Object>();
    mPersistentHandle.Reset(obj);
    async_resource = new AsyncResource("nsfw:Callbacks", obj);
    mInterfaceLockValid = uv_mutex_init(&mInterfaceLock) == 0;
  }

NSFW::~NSFW() {
  if (mInterface != NULL) {
    delete mInterface;
  }
  delete mEventCallback;
  delete mErrorCallback;
  delete async_resource;

  if (mInterfaceLockValid) {
    uv_mutex_destroy(&mInterfaceLock);
  }
}

void NSFW::fireErrorCallback(uv_async_t *handle) {
  Nan::HandleScope scope;
  ErrorBaton *baton = (ErrorBaton *)handle->data;
  v8::Local<v8::Value> argv[] = {
    New<v8::String>(baton->error).ToLocalChecked()
  };
  baton->nsfw->mErrorCallback->Call(1, argv, baton->nsfw->async_resource);
  delete baton;
}

void NSFW::fireEventCallback(uv_async_t *handle) {
  Nan::HandleScope scope;
  NSFW *nsfw = (NSFW *)handle->data;
  auto events = nsfw->mQueue->dequeueAll();
  if (events == nullptr) {
    return;
  }

  v8::Local<v8::Array> eventArray = New<v8::Array>((int)events->size());
  v8::Local<v8::Context> context = Nan::GetCurrentContext();

  for (unsigned int i = 0; i < events->size(); ++i) {
    v8::Local<v8::Object> jsEvent = New<v8::Object>();


    jsEvent->Set(context, New<v8::String>("action").ToLocalChecked(), New<v8::Number>((*events)[i]->type)).ToChecked();
    jsEvent->Set(context, New<v8::String>("directory").ToLocalChecked(), New<v8::String>((*events)[i]->fromDirectory).ToLocalChecked()).ToChecked();

    if ((*events)[i]->type == RENAMED) {
      jsEvent->Set(context, New<v8::String>("oldFile").ToLocalChecked(), New<v8::String>((*events)[i]->fromFile).ToLocalChecked()).ToChecked();
      jsEvent->Set(context, New<v8::String>("newDirectory").ToLocalChecked(), New<v8::String>((*events)[i]->toDirectory).ToLocalChecked()).ToChecked();
      jsEvent->Set(context, New<v8::String>("newFile").ToLocalChecked(), New<v8::String>((*events)[i]->toFile).ToLocalChecked()).ToChecked();
    } else {
      jsEvent->Set(context, New<v8::String>("file").ToLocalChecked(), New<v8::String>((*events)[i]->fromFile).ToLocalChecked()).ToChecked();
    }

    eventArray->Set(context, i, jsEvent).ToChecked();
  }

  v8::Local<v8::Value> argv[] = {
    eventArray
  };

  nsfw->mEventCallback->Call(1, argv, nsfw->async_resource);
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

    if (nsfw->mQueue->count() == 0) {
      uv_mutex_unlock(&nsfw->mInterfaceLock);
      sleep_for_ms(50);
      continue;
    }

    nsfw->mEventCallbackAsync.data = (void *)nsfw;
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

  v8::Local<v8::Context> context = Nan::GetCurrentContext();
  constructor.Reset(tpl->GetFunction(context).ToLocalChecked());
  Set(target, New<v8::String>("NSFW").ToLocalChecked(), tpl->GetFunction(context).ToLocalChecked());
}

NAN_METHOD(NSFW::JSNew) {
  if (!info.IsConstructCall()) {
    const int argc = 4;
    v8::Isolate *isolate = info.GetIsolate();
    v8::Local<v8::Value> argv[argc] = {info[0], info[1], info[2], info[3]};
    v8::Local<v8::Context> context = isolate->GetCurrentContext();
    v8::Local<v8::Function> cons = v8::Local<v8::Function>::New(isolate, constructor);
    info.GetReturnValue().Set(cons->NewInstance(context, argc, argv).ToLocalChecked());
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

  v8::Local<v8::Context> context = Nan::GetCurrentContext();
  uint32_t debounceMS = info[0]->Uint32Value(context).FromJust();
  Nan::Utf8String utf8Value(Nan::To<v8::String>(info[1]).ToLocalChecked());
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
    callback->Call(1, argv, nsfw->async_resource);
    delete callback;
    return;
  }

  New(nsfw->mPersistentHandle)->Set(Nan::GetCurrentContext(), New("nsfw").ToLocalChecked(), info.This()).ToChecked();

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

  mNSFW->mQueue->clear();
  mNSFW->mInterface = new NativeInterface(mNSFW->mPath, mNSFW->mQueue);
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
    callback->Call(1, argv, async_resource);
  } else {
    callback->Call(0, NULL, async_resource);
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
    callback->Call(1, argv, nsfw->async_resource);
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
  uv_mutex_unlock(&mNSFW->mInterfaceLock);

  // unlock the mInterfaceLock mutex while operate on the running identifier
  mNSFW->mRunning = false;

  uv_thread_join(&mNSFW->mPollThread);

  uv_mutex_lock(&mNSFW->mInterfaceLock);
  delete mNSFW->mInterface;
  mNSFW->mInterface = NULL;
  mNSFW->mQueue->clear();

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

  callback->Call(0, NULL, async_resource);
}

NODE_MODULE(nsfw, NSFW::Init)
