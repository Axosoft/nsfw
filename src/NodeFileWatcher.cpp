#include <nan.h>
#include <include/FileWatcher.h>

using v8::FunctionTemplate;
using v8::Handle;
using v8::Object;
using v8::String;
using Nan::New;
using Nan::Set;

NAN_MODULE_INIT(InitAll) {
  Set(target, New<String>("setAddCallback").ToLocalChecked(),
    New<FunctionTemplate>(SetAddCallback)->GetFunction());
  Set(target, New<String>("setDeleteCallback").ToLocalChecked(),
    New<FunctionTemplate>(SetDeleteCallback)->GetFunction());
  Set(target, New<String>("setModifyCallback").ToLocalChecked(),
    New<FunctionTemplate>(SetModifyCallback)->GetFunction());
}
