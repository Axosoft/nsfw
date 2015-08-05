#ifndef NodeSentinelFileWatcher_H
#define NodeSentinelFileWatcher_H

#include "FileWatcher.h"
#include <nan.h>
#include <queue>

namespace NSFW
{
  using namespace Nan;
  struct Event {
    std::string action;
    std::string directory;
    std::string file;
  };

  class NodeSentinelFileWatcher : public ObjectWrap {
  public:
    static NAN_MODULE_INIT(Init);

    // public members
    Callback *mCallback;
    FileWatcher* mFileWatcher;
    std::queue<Event> mEventQueue;

  private:
    // Constructors
    NodeSentinelFileWatcher(std::string path, Callback *pCallback);
    ~NodeSentinelFileWatcher();

    // Internal methods
    void enqueueEvent(const std::string &directory, const std::string &file, const std::string &action);

    // Javascript methods
    static NAN_METHOD(JSNew);
    static NAN_METHOD(Update);
    // Update worker
    class UpdateWorker : public AsyncWorker {
    public:
      // constructors
      UpdateWorker(FileWatcher * const fw, std::queue<Event> &eventQueue, Callback *callback);
      // Internal methods
      void Execute();
      void HandleOKCallback();

    private:
      // Internal members
      FileWatcher *mCallerFileWatcher;
      std::queue<Event> &mEventQueue;
    };

    // Nan necessary
    static Persistent<v8::Function> constructor;
  };
}

#endif
