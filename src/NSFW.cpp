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

NSFW::NSFW(uint32_t debounceMS, std::string path, Callback *eventCallback):
  mDebounceMS(debounceMS), mEventCallback(eventCallback), mInterface(NULL), mPath(path), mRunning(false) {}

NSFW::~NSFW() {
  if (mInterface != NULL) {
    delete mInterface;
  }
  delete mEventCallback;
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

void NSFW::fireEventCallback(uv_async_t *handle) {
  Nan::HandleScope scope;
  EventBaton *baton = (EventBaton *)handle->data;
  if (baton->events->empty()) {
    v8::Local<v8::Value> argv[] = { New<v8::Array>(0) };

    baton->nsfw->mEventCallback->Call(1, argv);
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
    std::vector<Event *> *events = nsfw->mInterface->getEvents();
    if (events == NULL) {
      continue;
    }

    uv_async_t async;
    uv_async_init(uv_default_loop(), &async, &NSFW::fireEventCallback);

    EventBaton *baton = new EventBaton;
    baton->nsfw = nsfw;
    baton->events = events;

    async.data = (void *)baton;
    uv_async_send(&async);

    sleep_for_ms(nsfw->mDebounceMS);
  }
  delete nsfw->mInterface;
  nsfw->mInterface = NULL;
}

NAN_MODULE_INIT(NSFW::Init) {
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
    return ThrowError("First argument of constructor must be a path.");
  }
  if (info.Length() < 2 || !info[1]->IsString()) {
    return ThrowError("First argument of constructor must be a path.");
  }
  if (info.Length() < 3 || !info[2]->IsFunction()) {
    return ThrowError("Second argument of constructor must be a callback.");
  }

  uint32_t debounceMS = info[0]->Uint32Value();
  v8::String::Utf8Value utf8Value(info[1]->ToString());
  std::string path = std::string(*utf8Value);
  Callback *callback = new Callback(info[2].As<v8::Function>());

  NSFW *nsfw = new NSFW(debounceMS, path, callback);
  nsfw->Wrap(info.This());
  info.GetReturnValue().Set(info.This());
}

NAN_METHOD(NSFW::Start) {
  if (
    info.Length() < 1 ||
    !info[0]->IsFunction()
  ) {
    return ThrowError("Must provide callback to start.");
  }

  NSFW *nsfw = ObjectWrap::Unwrap<NSFW>(info.This());
  Callback *callback = new Callback(info[0].As<v8::Function>());

  if (nsfw->mInterface != NULL) {
    v8::Local<v8::Value> argv[1] = {
      Nan::Error("This NSFW cannot be started, because it is already running.")
    };
    callback->Call(1, argv);
    return;
  }

  AsyncQueueWorker(new StartWorker(nsfw, callback));
}

NSFW::StartWorker::StartWorker(NSFW *nsfw, Callback *callback):
  AsyncWorker(callback), mNSFW(nsfw) {}

void NSFW::StartWorker::Execute() {
  mNSFW->mInterface = new NativeInterface(mNSFW->mPath);
  // TODO: check to see if it was actually started
  mNSFW->mRunning = true;
  uv_thread_create(&mNSFW->mPollThread, NSFW::pollForEvents, mNSFW);
}

void NSFW::StartWorker::HandleOKCallback() {
  HandleScope();
  callback->Call(0, NULL);
}

NAN_METHOD(NSFW::Stop) {
  if (
    info.Length() < 1 ||
    !info[0]->IsFunction()
  ) {
    return ThrowError("Must provide callback to stop.");
  }

  NSFW *nsfw = ObjectWrap::Unwrap<NSFW>(info.This());
  Callback *callback = new Callback(info[0].As<v8::Function>());

  if (nsfw->mInterface == NULL) {
    v8::Local<v8::Value> argv[1] = {
      Nan::Error("This NSFW cannot be stopped, because it is not running.")
    };
    callback->Call(1, argv);
    return;
  }

  AsyncQueueWorker(new StopWorker(nsfw, callback));
}

NSFW::StopWorker::StopWorker(NSFW *nsfw, Callback *callback):
  AsyncWorker(callback), mNSFW(nsfw) {}

void NSFW::StopWorker::Execute() {
  mNSFW->mRunning = false;
  uv_thread_join(&mNSFW->mPollThread);
}

void NSFW::StopWorker::HandleOKCallback() {
  HandleScope();
  callback->Call(0, NULL);
}

NODE_MODULE(nsfw, NSFW::Init)
