#ifndef NodeSentinelFileWatcher_H
#define NodeSentinelFileWatcher_H

#include "FileWatcher.h"
#include <nan.h>
#include <vector>

namespace NSFW
{
  using namespace Nan;

  class NodeSentinelFileWatcher : public ObjectWrap
  {
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
    static NAN_METHOD(Start);
    static NAN_METHOD(Stop);
    // Stop worker
    class StopWorker : public AsyncWorker
    {
    public:
      StopWorker(FileWatcher * const fw, Callback *callback);
      void Execute();
      void HandleOKCallback();
    private:
      FileWatcher * const mCallerFileWatcher;
    };

    // Nan necessary
    static Persistent<v8::Function> constructor;
  };
}

#endif
