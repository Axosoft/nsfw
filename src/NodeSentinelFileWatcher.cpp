#include "../includes/NodeSentinelFileWatcher.h"
namespace NSFW {

  Persistent<v8::Function> NodeSentinelFileWatcher::constructor;

  NodeSentinelFileWatcher::NodeSentinelFileWatcher(std::string path, Callback *pCallback) {
    mFileWatcher = new FileWatcher(path);
    mCallback = pCallback;
  }

  NodeSentinelFileWatcher::~NodeSentinelFileWatcher() {
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
    // if it's not running, don't try polling
    if (!nsfw->mFileWatcher->running()) return;

    std::queue<Event> *events = nsfw->mFileWatcher->pollEvents();
    while(!events->empty()) {
      Event event = events->front();
      events->pop();

      if (event.action == "RENAMED") {
        v8::Local<v8::Value> argv[] = {
          New<v8::String>(event.action).ToLocalChecked(),
          New<v8::String>(event.directory).ToLocalChecked(),
          New<v8::String>(event.file[0]).ToLocalChecked(),
          New<v8::String>(event.file[1]).ToLocalChecked()
        };

        nsfw->mCallback->Call(4, argv);
      } else {
        v8::Local<v8::Value> argv[] = {
          New<v8::String>(event.action).ToLocalChecked(),
          New<v8::String>(event.directory).ToLocalChecked(),
          New<v8::String>(*event.file).ToLocalChecked()
        };

        nsfw->mCallback->Call(3, argv);
      }
    }
  }

  NAN_METHOD(NodeSentinelFileWatcher::Start) {
    NodeSentinelFileWatcher *nsfw = ObjectWrap::Unwrap<NodeSentinelFileWatcher>(info.This());
    if (!nsfw->mFileWatcher->start()) {
      return ThrowError("Cannot start an already running NSFW.");
    }  }

  NAN_METHOD(NodeSentinelFileWatcher::Stop) {
    NodeSentinelFileWatcher *nsfw = ObjectWrap::Unwrap<NodeSentinelFileWatcher>(info.This());
    if (!nsfw->mFileWatcher->stop()) {
      return ThrowError("Cannot stop an already stopped NSFW.");
    }
  }

  // NodeSentinelFileWatcher::PollWorker ---------------------------------------

  NodeSentinelFileWatcher::PollWorker::PollWorker(FileWatcher * const fw, Callback *callback)
    : AsyncWorker(callback), mCallerFileWatcher(fw) {}

  void NodeSentinelFileWatcher::PollWorker::Execute() {
  }

  void NodeSentinelFileWatcher::PollWorker::HandleOKCallback() {
    HandleScope();

    std::queue<Event> *events = mCallerFileWatcher->pollEvents();
    while(!events->empty()) {
      Event event = events->front();
      events->pop();

      if (event.action == "RENAMED") {
        v8::Local<v8::Value> argv[] = {
          New<v8::String>(event.action).ToLocalChecked(),
          New<v8::String>(event.directory).ToLocalChecked(),
          New<v8::String>(event.file[0]).ToLocalChecked(),
          New<v8::String>(event.file[1]).ToLocalChecked()
        };

        callback->Call(4, argv);
      } else {
        v8::Local<v8::Value> argv[] = {
          New<v8::String>(event.action).ToLocalChecked(),
          New<v8::String>(event.directory).ToLocalChecked(),
          New<v8::String>(*event.file).ToLocalChecked()
        };

        callback->Call(3, argv);
      }
    }
  }

  NODE_MODULE(FileWatcher, NodeSentinelFileWatcher::Init)
}
