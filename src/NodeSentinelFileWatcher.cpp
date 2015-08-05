#include "../includes/NodeSentinelFileWatcher.h"

namespace NSFW {

  Persistent<v8::Function> NodeSentinelFileWatcher::constructor;

  NodeSentinelFileWatcher::NodeSentinelFileWatcher(std::string path, Callback *pCallback) {
    mFileWatcher = new FileWatcher();
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

    v8::Local<v8::ObjectTemplate> proto = tpl->PrototypeTemplate();

    SetPrototypeMethod(tpl, "update", Update);

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

  NAN_METHOD(NodeSentinelFileWatcher::Update) {
    NodeSentinelFileWatcher *nsfw = ObjectWrap::Unwrap<NodeSentinelFileWatcher>(info.This());
    AsyncQueueWorker(new UpdateWorker(nsfw->mFileWatcher, nsfw->mEventQueue, new Callback(nsfw->mCallback->GetFunction())));
  }

  // NodeSentinelFileWatcher::UpdateWorker ---------------------------------------

  NodeSentinelFileWatcher::UpdateWorker::UpdateWorker(FileWatcher * const fw, std::queue<Event> &eventQueue, Callback *callback)
    : AsyncWorker(callback), mCallerFileWatcher(fw), mEventQueue(eventQueue) {}

  void NodeSentinelFileWatcher::enqueueEvent(const std::string &directory, const std::string &file, const std::string &action) {
    Event event;
    event.directory = directory;
    event.file = file;
    event.action = action;
    mEventQueue.push(event);
  }

  void NodeSentinelFileWatcher::UpdateWorker::Execute() {
  }

  void NodeSentinelFileWatcher::UpdateWorker::HandleOKCallback() {
    HandleScope();

    std::queue<Event> events(mEventQueue);
    std::queue<Event> empty;
    std::swap(mEventQueue, empty);

    while(!events.empty()) {
      Event event = events.front();
      events.pop();

      v8::Local<v8::Value> argv[] = {
        New<v8::String>(event.action).ToLocalChecked(),
        New<v8::String>(event.directory).ToLocalChecked(),
        New<v8::String>(event.file).ToLocalChecked()
      };

      callback->Call(3, argv);
    }
  }

  NODE_MODULE(FileWatcher, NodeSentinelFileWatcher::Init)
}
