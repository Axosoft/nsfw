#include "../../includes/linux/InotifyEventLoop.h"

InotifyEventLoop::InotifyEventLoop(
  int inotifyInstance,
  InotifyService *inotifyService
) :
  mInotifyInstance(inotifyInstance),
  mInotifyService(inotifyService)
{
  mStarted = !pthread_create(
    &mEventLoop,
    NULL,
    [](void *eventLoop)->void * {
      ((InotifyEventLoop *)eventLoop)->work();
      return NULL;
    },
    (void *)this
  );
    if (mStarted) { mLoopingSemaphore.wait(); }
}

bool InotifyEventLoop::isStarted() {
  return mStarted;
}

std::unique_lock<std::mutex> InotifyEventLoop::lockEventHandling() {
  return std::unique_lock<std::mutex>(mMutex);
}

static std::string readErrorMessage(ssize_t bytesRead, int errorNumber) {
  if (bytesRead == 0) {
    return "Inotify read failed: unexpected end of file";
  }
  return "Inotify read failed: " + std::generic_category().message(errorNumber);
}

void InotifyEventLoop::work() {
  char buffer[BUFFER_SIZE];
  inotify_event *event = NULL;
  bool isDirectoryEvent = false, isDirectoryRemoval = false;
  InotifyService *inotifyService = mInotifyService;
  InotifyRenameEvent renameEvent;
  renameEvent.isStarted = false;

  auto create = [&event, &isDirectoryEvent, &inotifyService]() {
    if (event == NULL) {
      return;
    }

    if (isDirectoryEvent) {
      inotifyService->createDirectory(event->wd, event->len > 0 ? event->name : "");
    } else {
      inotifyService->create(event->wd, event->len > 0 ? event->name : "");
    }
  };

  auto modify = [&event, &isDirectoryEvent, &inotifyService]() {
    if (event == NULL) {
      return;
    }

    inotifyService->modify(event->wd, event->len > 0 ? event->name : "");
  };

  auto remove = [&event, &isDirectoryRemoval, &inotifyService]() {
    if (event == NULL) {
      return;
    }

    if (isDirectoryRemoval) {
      inotifyService->removeDirectory(event->wd);
    } else {
      inotifyService->remove(event->wd, event->len > 0 ? event->name : "");
    }
  };

  auto renameStart = [&event, &isDirectoryEvent, &renameEvent]() {
    renameEvent.cookie = event->cookie;
    renameEvent.isDirectory = isDirectoryEvent;
    renameEvent.name = event->len > 0 ? event->name : "";
    renameEvent.wd = event->wd;
    renameEvent.isStarted = true;
  };

  auto renameEnd = [&create, &event, &inotifyService, &isDirectoryRemoval, &renameEvent]() {
    if (!renameEvent.isStarted) {
      create();
      return;
    }

    if (renameEvent.cookie != event->cookie) {
      if (isDirectoryRemoval) {
        inotifyService->removeDirectory(renameEvent.wd);
      } else {
        inotifyService->remove(renameEvent.wd, renameEvent.name);
      }
      create();
    } else {
      if (renameEvent.isDirectory) {
        inotifyService->renameDirectory(renameEvent.wd, renameEvent.name, event->wd, event->len > 0 ? event->name : "");
      } else {
        inotifyService->rename(renameEvent.wd, renameEvent.name, event->wd, event->len > 0 ? event->name : "");
      }
    }
    renameEvent.isStarted = false;
  };

  mLoopingSemaphore.signal();
  std::string error;
  for (;;) {
    // The cancellation point the destructor relies on, so it must stay outside mMutex.
    ssize_t bytesRead = read(mInotifyInstance, &buffer, BUFFER_SIZE);
    if (bytesRead == -1 && errno == EINTR) {
      continue;
    }
    if (bytesRead <= 0) {
      error = readErrorMessage(bytesRead, errno);
      break;
    }

    std::lock_guard<std::mutex> syncWithDestructor(mMutex);
    ssize_t position = 0;
    do {
      event = (struct inotify_event *)(buffer + position);

      // The kernel queues this (with wd -1) once the queue holds fs.inotify.max_queued_events events, and drops the
      // events that follow, so the tree may be missing directories and the watcher can't be trusted anymore. Check it
      // first, so it never completes a pending rename or reaches a handler.
      if (event->mask & (uint32_t)IN_Q_OVERFLOW) {
        error = "Inotify event queue overflowed, events were lost";
        break;
      }

      if (renameEvent.isStarted && event->cookie != renameEvent.cookie) {
        renameEnd();
      }

      isDirectoryRemoval = event->mask & (uint32_t)(IN_IGNORED | IN_DELETE_SELF);
      isDirectoryEvent = event->mask & (uint32_t)(IN_ISDIR);

      if (event->mask & (uint32_t)(IN_ATTRIB | IN_MODIFY)) {
        modify();
      } else if (event->mask & (uint32_t)IN_CREATE) {
        create();
      } else if (event->mask & (uint32_t)(IN_DELETE | IN_DELETE_SELF)) {
        remove();
      } else if (event->mask & (uint32_t)IN_MOVED_TO) {
        if (event->cookie == 0) {
          create();
          continue;
        }

        renameEnd();
      } else if (event->mask & (uint32_t)IN_MOVED_FROM) {
        if (event->cookie == 0) {
          remove();
          continue;
        }

        renameStart();
      } else if (event->mask & (uint32_t)IN_MOVE_SELF) {
        inotifyService->removeDirectory(event->wd);
      }
    } while((position += sizeof(struct inotify_event) + event->len) < bytesRead);
    if (!error.empty()) {
      // The run is about to end with this error and the events still queued are discarded, so a pending rename is
      // dropped instead of being reported as a removal.
      break;
    }
    if (renameEvent.isStarted) {
      remove();
      renameEvent.isStarted = false;
    }
  }

  // Errors are terminal: the poll thread reports this one (or the one recorded before it, or the root path error if the
  // watched path is gone, see InotifyTree::getError()) through errorCallback and stops the watcher. isStarted() stays
  // true, see its declaration.
  inotifyService->setError(error);
}

InotifyEventLoop::~InotifyEventLoop() {
  if (!mStarted) {
    return;
  }

  {
    std::lock_guard<std::mutex> syncWithWork(mMutex);
    pthread_cancel(mEventLoop);
  }

  pthread_join(mEventLoop, NULL);
}
