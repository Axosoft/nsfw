#ifndef NodeSentinelFileWatcher_H
#define NodeSentinelFileWatcher_H

#include <nan.h>
#include "FileWatcher.h"
#include <queue>

using namespace Nan;

class NodeSentinelFileWatcher : public ObjectWrap {
public:
  static NAN_MODULE_INIT(Init);

  // public members
  Callback *mCallback;

private:
  // Constructors
  NodeSentinelFileWatcher(Callback *cb);
  NodeSentinelFileWatcher(Callback *cb, std::string paths[], int pathsCount);
  ~NodeSentinelFileWatcher();

  // Javascript methods
  static NAN_METHOD(AddWatch);
  static NAN_METHOD(JSNew);
  static NAN_METHOD(RemoveWatch);
  static NAN_METHOD(Update);
  // Update worker
  class NodeSentinelUpdateWorker : public AsyncWorker {
  public:
    // constructors
    NodeSentinelUpdateWorker(Callback *callback);
    // Internal Methods
    void Execute();
    void HandleOKCallback();

  private:
    // Internal members
    std::queue<std::string> mEvents;
  };

  // Internal members
  FW::FileWatcher* mFileWatcher;

  // Nan necessary
  static Persistent<v8::Function> constructor;
};

#endif
