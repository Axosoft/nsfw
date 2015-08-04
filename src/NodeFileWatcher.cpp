#include <nan.h>
#include <uv.h>
#include "../includes/NodeFileWatcher.h"
#include <iostream>

using namespace Nan;

Persistent<v8::Function> NodeFileWatcher::constructor;
NodeFileWatcher::NodeFileWatcher(int pInterval, Callback *cb){
  mFileWatcher = new FW::FileWatcher();
}

NodeFileWatcher::NodeFileWatcher(int pInterval, Callback *cb, std::string paths[], int pathsCount){
  mFileWatcher = new FW::FileWatcher();
}

NodeFileWatcher::~NodeFileWatcher(){
  delete mFileWatcher;
}

void NodeFileWatcher::mainLoop() {

}

NAN_MODULE_INIT(NodeFileWatcher::Init) {
  v8::Local<v8::FunctionTemplate> tpl = New<v8::FunctionTemplate>(JSNew);
  tpl->SetClassName(New<v8::String>("FileWatcher").ToLocalChecked());
  tpl->InstanceTemplate()->SetInternalFieldCount(1);

  v8::Local<v8::ObjectTemplate> proto = tpl->PrototypeTemplate();

  SetPrototypeMethod(tpl, "addWatch", AddWatch);
  SetPrototypeMethod(tpl, "removeWatch", RemoveWatch);
  //SetPrototypeTemplate(tpl, "interval", New<v8::String>("The interval at which the file system should be pinged for changes.").ToLocalChecked());
  SetAccessor(proto, New("interval").ToLocalChecked(), GetInterval, SetInterval);

  constructor.Reset(tpl->GetFunction());
  Set(target, New<v8::String>("FileWatcher").ToLocalChecked(), tpl->GetFunction());
}

NAN_METHOD(NodeFileWatcher::JSNew) {
  if (info.IsConstructCall()) {
    if (info.Length() < 2) {
      // blow up
    }
    if (!info[0]->IsUint32()) {
      // blow up
    }
    if (!info[1]->IsFunction()) {
      // blow up
    }
    int interval = info[0]->Int32Value();
    Callback *cb = new Callback(info[1].As<v8::Function>());
    if (info.Length() == 2) {
      NodeFileWatcher* nfw = new NodeFileWatcher(interval, cb);
      nfw->Wrap(info.This());
      info.GetReturnValue().Set(info.This());
    } else if (info.Length() == 3 && info[2]->IsArray()) {
      NodeFileWatcher* nfw = new NodeFileWatcher(interval, cb);
      nfw->Wrap(info.This());
      info.GetReturnValue().Set(info.This());
    } else {
      // blow up
    }
  } else {
    v8::Local<v8::Function> cons = New<v8::Function>(constructor);
    info.GetReturnValue().Set(cons->NewInstance());
  }
}

NAN_METHOD(NodeFileWatcher::AddWatch) {
  NodeFileWatcher* nfw = ObjectWrap::Unwrap<NodeFileWatcher>(info.This());
  std::cout << "Add watch" << std::endl;
}

NAN_GETTER(NodeFileWatcher::GetInterval) {
  NodeFileWatcher* nfw = ObjectWrap::Unwrap<NodeFileWatcher>(info.This());
  info.GetReturnValue().Set(New(nfw->mInterval));
}

NAN_METHOD(NodeFileWatcher::RemoveWatch) {
  NodeFileWatcher* nfw = ObjectWrap::Unwrap<NodeFileWatcher>(info.This());
  std::cout << "Remove watch" << std::endl;

}

NAN_SETTER(NodeFileWatcher::SetInterval) {
  NodeFileWatcher* nfw = ObjectWrap::Unwrap<NodeFileWatcher>(info.This());
  nfw->mInterval = value->Uint32Value();
}
// Always at the end
NODE_MODULE(FileWatcher, NodeFileWatcher::Init)
