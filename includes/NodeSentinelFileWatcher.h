#ifndef NodeSentinelFileWatcher_H
#define NodeSentinelFileWatcher_H

#include "FileWatcher.h"
#include <nan.h>
#include <queue>

namespace NSFW
{
  using namespace Nan;

  class NodeSentinelFileWatcher : public ObjectWrap {
  public:
    static NAN_MODULE_INIT(Init);

    // public members
    Callback *mCallback;
    FileWatcher* mFileWatcher;

  private:
    // Constructors
    NodeSentinelFileWatcher(std::string path, Callback *pCallback);
    ~NodeSentinelFileWatcher();

    // Javascript methods
    static NAN_METHOD(JSNew);
    static NAN_METHOD(Poll);
    // Poll worker
    class PollWorker : public AsyncWorker {
    public:
      // constructors
      PollWorker(FileWatcher * const fw, Callback *callback);
      // Internal methods
      void Execute();
      void HandleOKCallback();

    private:
      // Internal members
      FileWatcher *mCallerFileWatcher;
    };

    // Nan necessary
    static Persistent<v8::Function> constructor;
  };
}

#endif
