#include "../includes/NodeSentinelFileWatcher.h"
#if defined(_WIN32)
#include <windows.h>
#elif defined(__APPLE_CC__) || defined(BSD) || defined(__linux__)
#include <unistd.h>
#endif

// #include <iostream>

namespace NSFW {

  #pragma unmanaged
  Persistent<v8::Function> NodeSentinelFileWatcher::constructor;


  NodeSentinelFileWatcher::NodeSentinelFileWatcher(std::string path, Callback *pCallback) {
    mFileWatcher = new FileWatcher(path);
    mCallback = pCallback;
  }

  NodeSentinelFileWatcher::~NodeSentinelFileWatcher() {
    // std::cout << "gc start" << std::endl;
    mFileWatcher->stop();
    // std::cout << "gc finish" << std::endl;
    delete mFileWatcher;
    delete mCallback;
  }

  NAN_MODULE_INIT(NodeSentinelFileWatcher::Init) {
    v8::Local<v8::FunctionTemplate> tpl = New<v8::FunctionTemplate>(JSNew);
    tpl->SetClassName(New<v8::String>("NSFW").ToLocalChecked());
    tpl->InstanceTemplate()->SetInternalFieldCount(1);

    SetPrototypeMethod(tpl, "poll", Poll);
    SetPrototypeMethod(tpl, "start", Start);
    SetPrototypeMethod(tpl, "stop", Stop);

    constructor.Reset(tpl->GetFunction());
    Set(target, New<v8::String>("NSFW").ToLocalChecked(), tpl->GetFunction());
  }

  NAN_METHOD(NodeSentinelFileWatcher::JSNew) {
    if (!info.IsConstructCall()) {
      v8::Local<v8::Function> cons = New<v8::Function>(constructor);
      info.GetReturnValue().Set(cons->NewInstance());
      return;
    }

    if (info.Length() < 1 || !info[0]->IsString()) {
      return ThrowError("First argument of constructor must be a path.");
    }
    if (info.Length() < 2 || !info[1]->IsFunction()) {
      return ThrowError("Second argument of constructor must be a callback.");
    }
    // prepare the arguments to pass to the constructor
    v8::String::Utf8Value utf8Value(info[0]->ToString());
    std::string path = std::string(*utf8Value);
    Callback *callback = new Callback(info[1].As<v8::Function>());

    NodeSentinelFileWatcher *nsfw = new NodeSentinelFileWatcher(path, callback);
    nsfw->Wrap(info.This());
    info.GetReturnValue().Set(info.This());
  }

  NAN_METHOD(NodeSentinelFileWatcher::Poll) {
    NodeSentinelFileWatcher *nsfw = ObjectWrap::Unwrap<NodeSentinelFileWatcher>(info.This());
    if (nsfw->mFileWatcher->errors()) {
      return ThrowError(
        New<v8::String>(nsfw->mFileWatcher->errorMessage()).ToLocalChecked()
      );
    }

    // if it's not running, don't try polling
    if (!nsfw->mFileWatcher->running()) return;

    std::queue<Event> *events = nsfw->mFileWatcher->pollEvents();

    if (events == NULL) {
      v8::Local<v8::Array> emptyArray = New<v8::Array>(0);

      v8::Local<v8::Value> argv[] = {
        emptyArray
      };

      nsfw->mCallback->Call(1, argv);
      return;
    }

    std::vector< v8::Local<v8::Object> > jsEventObjects;

    while(!events->empty()) {
      Event event = events->front();
      events->pop();

      v8::Local<v8::Object> anEvent = New<v8::Object>();

      std::string strAction;
      switch(event.action) {
      case CREATED:
        strAction = "CREATED";
        break;
      case DELETED:
        strAction = "DELETED";
        break;
      case MODIFIED:
        strAction = "CHANGED";
        break;
      case RENAMED:
        strAction = "RENAMED";
        break;
      }
      anEvent->Set(New<v8::String>("action").ToLocalChecked(), New<v8::String>(strAction).ToLocalChecked());
      anEvent->Set(New<v8::String>("directory").ToLocalChecked(), New<v8::String>(event.directory).ToLocalChecked());

      if (event.action == RENAMED) {
        anEvent->Set(New<v8::String>("oldFile").ToLocalChecked(), New<v8::String>(event.file[0]).ToLocalChecked());
        anEvent->Set(New<v8::String>("newFile").ToLocalChecked(), New<v8::String>(event.file[1]).ToLocalChecked());
      } else {
        anEvent->Set(New<v8::String>("file").ToLocalChecked(), New<v8::String>(event.file[0]).ToLocalChecked());
      }

      jsEventObjects.push_back(anEvent);
    }
    delete events;

    v8::Local<v8::Array> eventArray = New<v8::Array>((int)jsEventObjects.size());

    for (unsigned int i = 0; i < jsEventObjects.size(); ++i) {
      eventArray->Set(i, jsEventObjects[i]);
    }

    v8::Local<v8::Value> argv[] = {
      eventArray
    };

    nsfw->mCallback->Call(1, argv);
  }

  NAN_METHOD(NodeSentinelFileWatcher::Start) {
    NodeSentinelFileWatcher *nsfw = ObjectWrap::Unwrap<NodeSentinelFileWatcher>(info.This());
    if (!nsfw->mFileWatcher->start()) {
      return ThrowError("Cannot start an already running NSFW.");
    }
  }

  NAN_METHOD(NodeSentinelFileWatcher::Stop) {
    if (info.Length() < 1 || !info[0]->IsFunction()) {
      return ThrowError("Must provide a callback to stop.");
    }
    Callback *callback = new Callback(info[0].As<v8::Function>());

    NodeSentinelFileWatcher *nsfw = ObjectWrap::Unwrap<NodeSentinelFileWatcher>(info.This());
    if (!nsfw->mFileWatcher->stop()) {
      return ThrowError("Cannot stop an already stopped NSFW.");
    }
    AsyncQueueWorker(new StopWorker(nsfw->mFileWatcher, callback));
  }

  NodeSentinelFileWatcher::StopWorker::StopWorker(FileWatcher * const fw, Callback *callback)
    : AsyncWorker(callback), mCallerFileWatcher(fw) {}

  void NodeSentinelFileWatcher::StopWorker::Execute() {
    while(!mCallerFileWatcher->hasStopped()) {
      #if defined(_WIN32)
      Sleep(50);
      #elif defined(__APPLE_CC__) || defined(BSD) || defined(__linux__)
      usleep(50000);
      #endif
    }
  }

  void NodeSentinelFileWatcher::StopWorker::HandleOKCallback() {
    HandleScope();
    callback->Call(0, NULL);
  }

  NODE_MODULE(FileWatcher, NodeSentinelFileWatcher::Init)
}
