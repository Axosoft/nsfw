#include <nan.h>
#include "../includes/NodeFileWatcher.h"

using namespace Nan;

Persistent<v8::Function> NodeFileWatcher::constructor;
NodeFileWatcher::NodeFileWatcher(){}
NodeFileWatcher::~NodeFileWatcher(){}

NAN_MODULE_INIT(NodeFileWatcher::Init) {
  v8::Local<v8::FunctionTemplate> tpl = New<v8::FunctionTemplate>(JSNew);
  tpl->SetClassName(New<v8::String>("FileWatcher").ToLocalChecked());
  tpl->InstanceTemplate()->SetInternalFieldCount(1);

  SetPrototypeMethod(tpl, "addWatch", AddWatch);
  //SetPrototypeMethod(tpl, "removeWatch", New<FunctionTemplate>(RemoveWatch)->GetFunction());
  //SetAccessor(NanNew<String>("interval"), GetInterval, SetInterval);
  SetPrototypeTemplate(tpl, "interval", New<v8::String>("The interval at which the file system should be pinged for changes.").ToLocalChecked());

  constructor.Reset(tpl->GetFunction());
  Set(target, New<v8::String>("FileWatcher").ToLocalChecked(), tpl->GetFunction());
}

NAN_METHOD(NodeFileWatcher::JSNew) {
  if (info.IsConstructCall()) {
    NodeFileWatcher* nfw = new NodeFileWatcher();
    nfw->Wrap(info.This());
    info.GetReturnValue().Set(info.This());
  } else {
    v8::Local<v8::Function> cons = New<v8::Function>(constructor);
    info.GetReturnValue().Set(cons->NewInstance());
  }
}

NAN_METHOD(NodeFileWatcher::AddWatch) {
  NodeFileWatcher* nfw = ObjectWrap::Unwrap<NodeFileWatcher>(info.This());
  info.GetReturnValue().Set(New<v8::String>("Hello world!").ToLocalChecked());
}
// Always at the end
NODE_MODULE(FileWatcher, NodeFileWatcher::Init)
