#ifndef NodeSentinelFileWatcher_H
#define NodeSentinelFileWatcher_H

#include <nan.h>
#include "FileWatcher.h"
#include <queue>

using namespace Nan;

struct NSFWEvent {
  std::string dir;
  std::string filename;
  FW::Actions::Action action;
};

class NodeSentinelFileWatcher : public ObjectWrap {
public:
  static NAN_MODULE_INIT(Init);

  // public members
  Callback *mCallback;
  FW::FileWatcher* mFileWatcher;
  std::queue<NSFWEvent> mEventQueue;

private:
  // Constructors
  NodeSentinelFileWatcher(Callback *pCallback, std::string path);
  ~NodeSentinelFileWatcher();

  // Internal methods
  void enqueueEvent(const std::string &dir, const std::string &filename, FW::Actions::Action action);

  // Javascript methods
  static NAN_METHOD(JSNew);
  static NAN_METHOD(Update);
  // Update worker
  class UpdateWorker : public AsyncWorker {
  public:
    // constructors
    UpdateWorker(FW::FileWatcher * const fw, std::queue<NSFWEvent> &eventQueue, Callback *callback);
    // Internal methods
    void Execute();
    void HandleOKCallback();

  private:
    // Internal members
    FW::FileWatcher *mCallerFileWatcher;
    std::queue<NSFWEvent> &mEventQueue;
  };

  // update listener
  class UpdateListener : public FW::FileWatchListener {
  public:
    UpdateListener(NodeSentinelFileWatcher * const parent);
    void handleFileAction(FW::WatchID watchid, const std::string &dir, const std::string &filename, FW::Actions::Action action);
  private:
    NodeSentinelFileWatcher * const mParent;
  };

  // Nan necessary
  static Persistent<v8::Function> constructor;
};

#endif
