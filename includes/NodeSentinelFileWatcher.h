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
  FW::FileWatcher* mFileWatcher;

private:
  // Constructors
  NodeSentinelFileWatcher(Callback *cb);
  NodeSentinelFileWatcher(Callback *cb, std::vector<std::string> *paths);
  ~NodeSentinelFileWatcher();

  // Internal methods
  static bool toStringVector(std::vector<std::string> *stringVector, v8::Local<v8::Array> jsInputArray);

  // Javascript methods
  static NAN_METHOD(AddWatch);
  static NAN_METHOD(JSNew);
  static NAN_METHOD(RemoveWatch);
  static NAN_METHOD(Update);
  // Update worker
  class UpdateWorker : public AsyncWorker {
  public:
    // constructors
    UpdateWorker(FW::FileWatcher *fw, Callback *callback);
    // Internal methods
    void enqueueEvent(const std::string &dir, const std::string &filename, FW::Actions::Action action);
    void Execute();
    void HandleOKCallback();
    // Internal members
    FW::FileWatcher *mCallerFileWatcher;

  private:
    // Internal members
    std::queue<std::string> mEvents;
  };

  // update listener
  class UpdateListener : public FW::FileWatchListener {
  public:
    UpdateListener(NodeSentinelFileWatcher::UpdateWorker * const parent);
    void handleFileAction(FW::WatchID watchid, const std::string &dir, const std::string &filename, FW::Actions::Action action);
  private:
    NodeSentinelFileWatcher::UpdateWorker * const mParent;
  };

  // Nan necessary
  static Persistent<v8::Function> constructor;
};

#endif
