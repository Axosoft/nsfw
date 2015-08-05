#include "../includes/NodeSentinelFileWatcher.h"
#include <iostream>

Persistent<v8::Function> NodeSentinelFileWatcher::constructor;

NodeSentinelFileWatcher::NodeSentinelFileWatcher(Callback *pCallback) {
  mFileWatcher = new FW::FileWatcher();
  mCallback = pCallback;
}

NodeSentinelFileWatcher::NodeSentinelFileWatcher(Callback *pCallback, std::vector<std::string> *paths) {
  mFileWatcher = new FW::FileWatcher();
  mCallback = pCallback;

  // the directories to the filewatcher
  for (std::vector<std::string>::iterator iter = paths->begin(); iter != paths->end(); ++iter) {
    mFileWatcher->addWatch(*iter);
  }
}

NodeSentinelFileWatcher::~NodeSentinelFileWatcher() {
  delete mFileWatcher;
  delete mCallback;
}

bool NodeSentinelFileWatcher::toStringVector(std::vector<std::string> *stringVector, v8::Local<v8::Array> jsInputArray) {
  // it's a string array if it's an empty array.
  if (jsInputArray->Length() == 0) return true;

  // otherwise we need to confirm that it is indeed a string array
  for (int iter = 0; iter < jsInputArray->Length(); ++iter) {
    v8::Local<v8::Value> arrayObject = jsInputArray->Get(iter);
    if (arrayObject->IsString()) {
      //we need to do an intermediate conversion before we create a string internally
      v8::String::Utf8Value utf8Value(arrayObject->ToString());
      std::string cppString = std::string(*utf8Value);
      stringVector->push_back(cppString);
    } else {
      // return false if a single object is found that is not of type string.
      return false;
    }
  }
  // return true if all objects in the array were found to be strings
  return true;
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
  if (!info.IsConstructCall()) {
    v8::Local<v8::Function> cons = New<v8::Function>(constructor);
    info.GetReturnValue().Set(cons->NewInstance());
    return;
  }

  if (info.Length() < 1) {
    return ThrowRangeError("Constructor must be called with callback.");
  }
  if (!info[0]->IsFunction()) {
    return ThrowError("First argument must be a callback.");
  }
  // we are insured at least callbacks exists.
  Callback *callback = new Callback(info[0].As<v8::Function>());
  std::vector<std::string> *paths = new std::vector<std::string>();

  if (info.Length() == 1) {
    // if only callback is provided, construct with just callback
    NodeSentinelFileWatcher *nsfw = new NodeSentinelFileWatcher(callback);
    nsfw->Wrap(info.This());
    info.GetReturnValue().Set(info.This());
  } else if (info[1]->IsArray() && toStringVector(paths, info[1].As<v8::Array>())) {
    // else we were provided an array of paths trailing the callback
    NodeSentinelFileWatcher *nsfw = new NodeSentinelFileWatcher(callback, paths);
    nsfw->Wrap(info.This());
    info.GetReturnValue().Set(info.This());
  } else {
    // forced cleanup
    delete paths;
    return ThrowError("Second argument must be an array of paths.");
  }

  // normal clean up
  delete paths;
}

NAN_METHOD(NodeSentinelFileWatcher::AddWatch) {
  NodeSentinelFileWatcher *nsfw = ObjectWrap::Unwrap<NodeSentinelFileWatcher>(info.This());
}

NAN_METHOD(NodeSentinelFileWatcher::RemoveWatch) {
  NodeSentinelFileWatcher *nsfw = ObjectWrap::Unwrap<NodeSentinelFileWatcher>(info.This());
}

NAN_METHOD(NodeSentinelFileWatcher::Update) {
  NodeSentinelFileWatcher *nsfw = ObjectWrap::Unwrap<NodeSentinelFileWatcher>(info.This());
  AsyncQueueWorker(new UpdateWorker(nsfw->mFileWatcher, new Callback(nsfw->mCallback->GetFunction())));
}

// NodeSentinelFileWatcher::UpdateWorker ---------------------------------------

NodeSentinelFileWatcher::UpdateWorker::UpdateWorker(FW::FileWatcher *fw, Callback *callback)
  : AsyncWorker(callback), mCallerFileWatcher(fw) {}

void NodeSentinelFileWatcher::UpdateWorker::enqueueEvent(const std::string &dir, const std::string &filename, FW::Actions::Action action) {
  // this needs to be thread safe
}

void NodeSentinelFileWatcher::UpdateWorker::Execute() {
  mCallerFileWatcher->update(new NodeSentinelFileWatcher::UpdateListener(this));
}

void NodeSentinelFileWatcher::UpdateWorker::HandleOKCallback() {
  HandleScope();

  v8::Local<v8::Value> argv[] = {
    New<v8::String>("modified").ToLocalChecked(),
    New<v8::String>("/a/path").ToLocalChecked()
  };

  callback->Call(2, argv);
}

// NodeSentinelFileWatcher::UpdateListener -------------------------------------
NodeSentinelFileWatcher::UpdateListener::UpdateListener(NodeSentinelFileWatcher::UpdateWorker * const parent)
 : mParent(parent) {}

void NodeSentinelFileWatcher::UpdateListener::handleFileAction(FW::WatchID watchid, const std::string &dir, const std::string &filename, FW::Actions::Action action) {
  mParent->enqueueEvent(dir, filename, action);
}

NODE_MODULE(FileWatcher, NodeSentinelFileWatcher::Init)
