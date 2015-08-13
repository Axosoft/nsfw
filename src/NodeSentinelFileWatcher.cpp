#include "../includes/NodeSentinelFileWatcher.h"
namespace NSFW {

  Persistent<v8::Function> NodeSentinelFileWatcher::constructor;

  NodeSentinelFileWatcher::NodeSentinelFileWatcher(std::string path, Callback *pCallback) {
    mFileWatcher = new FileWatcher(path);
    mCallback = pCallback;
  }

  NodeSentinelFileWatcher::~NodeSentinelFileWatcher() {
    delete mFileWatcher;
    delete mCallback;
  }

  NAN_MODULE_INIT(NodeSentinelFileWatcher::Init) {
    v8::Local<v8::FunctionTemplate> tpl = New<v8::FunctionTemplate>(JSNew);
    tpl->SetClassName(New<v8::String>("NSFW").ToLocalChecked());
    tpl->InstanceTemplate()->SetInternalFieldCount(1);

    SetPrototypeMethod(tpl, "poll", Poll);
    SetPrototypeMethod(tpl, "start", Start);
    SetPrototypeMethod(tpl, "stop", Stop);

    constructor.Reset(tpl->GetFunction());
    Set(target, New<v8::String>("NSFW").ToLocalChecked(), tpl->GetFunction());
  }

  NAN_METHOD(NodeSentinelFileWatcher::JSNew) {
    if (!info.IsConstructCall()) {
      v8::Local<v8::Function> cons = New<v8::Function>(constructor);
      info.GetReturnValue().Set(cons->NewInstance());
      return;
    }

    if (info.Length() < 1 || !info[0]->IsString()) {
      return ThrowError("First argument of constructor must be a path.");
    }
    if (info.Length() < 2 || !info[1]->IsFunction()) {
      return ThrowError("Second argument of constructor must be a callback.");
    }
    // prepare the arguments to pass to the constructor
    v8::String::Utf8Value utf8Value(info[0]->ToString());
    std::string path = std::string(*utf8Value);
    Callback *callback = new Callback(info[1].As<v8::Function>());

    NodeSentinelFileWatcher *nsfw = new NodeSentinelFileWatcher(path, callback);
    nsfw->Wrap(info.This());
    info.GetReturnValue().Set(info.This());
  }

  NAN_METHOD(NodeSentinelFileWatcher::Poll) {
    NodeSentinelFileWatcher *nsfw = ObjectWrap::Unwrap<NodeSentinelFileWatcher>(info.This());
    // if it's not running, don't try polling
    if (!nsfw->mFileWatcher->running()) return;

    std::queue<Event> *events = nsfw->mFileWatcher->pollEvents();

    v8::Local<v8::Object> parsedEvents = New<v8::Object>();

    std::vector< v8::Local<v8::Object> > changedEvents, createdEvents, deletedEvents,  renamedEvents;

    while(!events->empty()) {
      Event event = events->front();
      events->pop();

      if (event.action == "RENAMED") {

        v8::Local<v8::Object> renamedEvent = New<v8::Object>();

        renamedEvent->Set(New<v8::String>("directory").ToLocalChecked(), New<v8::String>(event.directory).ToLocalChecked());
        renamedEvent->Set(New<v8::String>("oldFile").ToLocalChecked(), New<v8::String>(event.file[0]).ToLocalChecked());
        renamedEvent->Set(New<v8::String>("newFile").ToLocalChecked(), New<v8::String>(event.file[1]).ToLocalChecked());

        renamedEvents.push_back(renamedEvent);

        delete[] event.file;
      } else {

        v8::Local<v8::Object> anEvent = New<v8::Object>();

        anEvent->Set(New<v8::String>("directory").ToLocalChecked(), New<v8::String>(event.directory).ToLocalChecked());
        anEvent->Set(New<v8::String>("file").ToLocalChecked(), New<v8::String>(*event.file).ToLocalChecked());

        if (event.action == "CHANGED")
          changedEvents.push_back(anEvent);
        else if (event.action == "CREATED")
          createdEvents.push_back(anEvent);
        else
          deletedEvents.push_back(anEvent);

        delete event.file;
      }
    }

    v8::Local<v8::Array> changedArray = New<v8::Array>((int)changedEvents.size());
    v8::Local<v8::Array> createdArray = New<v8::Array>((int)createdEvents.size());
    v8::Local<v8::Array> deletedArray = New<v8::Array>((int)deletedEvents.size());
    v8::Local<v8::Array> renamedArray = New<v8::Array>((int)renamedEvents.size());

    for (unsigned int i = 0; i < changedEvents.size(); ++i) {
      changedArray->Set(i, changedEvents[i]);
    }

    for (unsigned int i = 0; i < createdEvents.size(); ++i) {
      createdArray->Set(i, createdEvents[i]);
    }

    for (unsigned int i = 0; i < deletedEvents.size(); ++i) {
      deletedArray->Set(i, deletedEvents[i]);
    }

    for (unsigned int i = 0; i < renamedEvents.size(); ++i) {
      renamedArray->Set(i, renamedEvents[i]);
    }

    parsedEvents->Set(New<v8::String>("changed").ToLocalChecked(), changedArray);
    parsedEvents->Set(New<v8::String>("created").ToLocalChecked(), createdArray);
    parsedEvents->Set(New<v8::String>("deleted").ToLocalChecked(), deletedArray);
    parsedEvents->Set(New<v8::String>("renamed").ToLocalChecked(), renamedArray);

    v8::Local<v8::Value> argv[] = {
      parsedEvents
    };

    nsfw->mCallback->Call(1, argv);
  }

  NAN_METHOD(NodeSentinelFileWatcher::Start) {
    NodeSentinelFileWatcher *nsfw = ObjectWrap::Unwrap<NodeSentinelFileWatcher>(info.This());
    if (!nsfw->mFileWatcher->start()) {
      return ThrowError("Cannot start an already running NSFW.");
    }  }

  NAN_METHOD(NodeSentinelFileWatcher::Stop) {
    NodeSentinelFileWatcher *nsfw = ObjectWrap::Unwrap<NodeSentinelFileWatcher>(info.This());
    if (!nsfw->mFileWatcher->stop()) {
      return ThrowError("Cannot stop an already stopped NSFW.");
    }
  }

  NODE_MODULE(FileWatcher, NodeSentinelFileWatcher::Init)
}
