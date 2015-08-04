#include <nan.h>
#include "../includes/NodeSentinelFileWatcher.h"
#include <iostream>

using namespace Nan;

Persistent<v8::Function> NodeSentinelFileWatcher::constructor;

NodeSentinelFileWatcher::NodeSentinelFileWatcher(Callback *pCallback) {
  mFileWatcher = new FW::FileWatcher();
  mCallback = pCallback;
}

NodeSentinelFileWatcher::NodeSentinelFileWatcher(Callback *pCallback, std::string paths[], int pathsCount) {
  mFileWatcher = new FW::FileWatcher();
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

  // add comments here
  SetPrototypeMethod(tpl, "addWatch", AddWatch);
  SetPrototypeMethod(tpl, "removeWatch", RemoveWatch);
  SetPrototypeMethod(tpl, "update", Update);

  constructor.Reset(tpl->GetFunction());
  Set(target, New<v8::String>("NSFW").ToLocalChecked(), tpl->GetFunction());
}

NAN_METHOD(NodeSentinelFileWatcher::JSNew) {
  if (info.IsConstructCall()) {
    if (info.Length() < 1) {
      return ThrowRangeError("Constructor must be called with callback.");
    }
    if (!info[0]->IsFunction()) {
      return ThrowError("First argument must be a callback.");
    }
    Callback *callback = new Callback(info[0].As<v8::Function>());
    if (info.Length() == 1) {
      NodeSentinelFileWatcher *nsfw = new NodeSentinelFileWatcher(callback);
      nsfw->Wrap(info.This());
      info.GetReturnValue().Set(info.This());
    } else if (info[1]->IsArray()) {
      NodeSentinelFileWatcher *nsfw = new NodeSentinelFileWatcher(callback);
      nsfw->Wrap(info.This());
      info.GetReturnValue().Set(info.This());
    } else {
      return ThrowError("Second argument must be an array of paths.");
    }
  } else {
    v8::Local<v8::Function> cons = New<v8::Function>(constructor);
    info.GetReturnValue().Set(cons->NewInstance());
  }
}

NAN_METHOD(NodeSentinelFileWatcher::AddWatch) {
  NodeSentinelFileWatcher *nsfw = ObjectWrap::Unwrap<NodeSentinelFileWatcher>(info.This());
}

NAN_METHOD(NodeSentinelFileWatcher::RemoveWatch) {
  NodeSentinelFileWatcher *nsfw = ObjectWrap::Unwrap<NodeSentinelFileWatcher>(info.This());
}

NAN_METHOD(NodeSentinelFileWatcher::Update) {
  NodeSentinelFileWatcher *nsfw = ObjectWrap::Unwrap<NodeSentinelFileWatcher>(info.This());
  AsyncQueueWorker(new NodeSentinelUpdateWorker(new Callback(nsfw->mCallback->GetFunction())));
}

NodeSentinelFileWatcher::NodeSentinelUpdateWorker::NodeSentinelUpdateWorker(Callback *callback) : AsyncWorker(callback) {}

void NodeSentinelFileWatcher::NodeSentinelUpdateWorker::Execute() {
  std::cout << "hello ";
}

void NodeSentinelFileWatcher::NodeSentinelUpdateWorker::HandleOKCallback() {
  HandleScope();

  v8::Local<v8::Value> argv[] = {
    New<v8::String>("modified").ToLocalChecked(),
    New<v8::String>("/a/path").ToLocalChecked()
  };

  callback->Call(2, argv);
}


NODE_MODULE(FileWatcher, NodeSentinelFileWatcher::Init)
