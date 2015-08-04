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
  // Constructors
  NodeFileWatcher(int pInterval, Callback *cb);
  NodeFileWatcher(int pInterval, Callback *cb, std::string paths[], int pathsCount);
  ~NodeFileWatcher();

  // Javascript methods
  static NAN_METHOD(AddWatch);
  static NAN_GETTER(GetInterval);
  static NAN_METHOD(JSNew);
  static NAN_METHOD(RemoveWatch);
  static NAN_SETTER(SetInterval);

  // Internal methods
  void mainLoop();

  // Internal members
  FW::FileWatcher* mFileWatcher;
  int mInterval;

  // Nan necessary
  static Persistent<v8::Function> constructor;
};

#endif
