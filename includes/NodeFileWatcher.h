#ifndef NODEFILEWATCHER_H
#define NODEFILEWATCHER_H

#include <nan.h>
#include "FileWatcher.h"
#include <set>

using namespace Nan;

class NodeFileWatcher : public ObjectWrap {
public:
  static NAN_MODULE_INIT(Init);
private:
  NodeFileWatcher();
  ~NodeFileWatcher();
  //set<string> mPaths;
  static NAN_METHOD(JSNew);
  static NAN_METHOD(AddWatch);
  static Persistent<v8::Function> constructor;

};

#endif
