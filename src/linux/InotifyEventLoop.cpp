#include "../../includes/linux/InotifyEventLoop.h"

#include <poll.h>

InotifyEventLoop::InotifyEventLoop(
  int inotifyInstance,
  InotifyService *inotifyService
) :
  mInotifyInstance(inotifyInstance),
  mInotifyService(inotifyService)
{
  if (pipe2(mWakePipe, O_CLOEXEC | O_NONBLOCK) == -1) return;
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

void InotifyEventLoop::wakeForEntryRetry() {
  if (!mInotifyService->mTree->needsEntryRetry()) return;
  ssize_t sent;
  do {
    sent = write(mWakePipe[1], "1", 1);
  } while (sent == -1 && errno == EINTR);
  if (sent == -1 && errno != EAGAIN) mInotifyService->setError("Cannot wake inotify reader");
}

void InotifyEventLoop::work() {
  char buffer[BUFFER_SIZE];
  inotify_event *event = NULL;
  bool isDirectoryEvent = false, isDirectoryRemoval = false;
  InotifyService *inotifyService = mInotifyService;
  InotifyRenameEvent renameEvent;
  renameEvent.isStarted = false;

  auto create = [&event, &isDirectoryEvent, &inotifyService](bool fromInCreate) {
    if (event == NULL) {
      return;
    }

    if (isDirectoryEvent) {
      inotifyService->createDirectory(event->wd, event->len > 0 ? event->name : "", fromInCreate);
    } else {
      inotifyService->create(event->wd, event->len > 0 ? event->name : "", fromInCreate);
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

  // A move whose IN_MOVED_TO didn't come: the entry most likely went somewhere that isn't watched (outside the watched
  // path, or an excluded directory). An IN_MOVED_TO that comes later anyway, after an event of another cookie or the
  // wait below, makes it a deletion and a creation. It only reads renameEvent, since the event that triggers it may be
  // in another buffer.
  auto renameAbandon = [&inotifyService, &renameEvent]() {
    if (!renameEvent.isStarted) {
      return;
    }

    renameEvent.isStarted = false;
    if (renameEvent.isDirectory) {
      inotifyService->removeMovedDirectory(renameEvent.wd, renameEvent.name);
    } else {
      inotifyService->remove(renameEvent.wd, renameEvent.name);
      inotifyService->revalidateLinks();
    }
  };

  // A pending rename is abandoned before any event with another cookie is handled, so one that is still started here
  // is this event's other half.
  auto renameEnd = [&create, &event, &inotifyService, &renameEvent]() {
    if (!renameEvent.isStarted) {
      // From a directory that isn't watched.
      create(false);
      return;
    }

    renameEvent.isStarted = false;
    if (renameEvent.isDirectory) {
      inotifyService->renameDirectory(renameEvent.wd, renameEvent.name, event->wd, event->len > 0 ? event->name : "");
    } else {
      inotifyService->rename(renameEvent.wd, renameEvent.name, event->wd, event->len > 0 ? event->name : "");
    }
  };

  // Whether the event is one of a watch taken back behind a marker not read yet, which is dropped: what it tells
  // about the entries of its directory goes with it, and the tree has to know that much.
  auto dropQuarantined = [&event, &inotifyService]() {
    if (!inotifyService->isQuarantined(event->wd, event->mask)) {
      return false;
    }
    if (event->mask & (uint32_t)(IN_CREATE | IN_DELETE | IN_MOVED_FROM | IN_MOVED_TO)) {
      inotifyService->entryDropped(event->wd, event->len > 0 ? event->name : "");
    }
    return true;
  };

  mLoopingSemaphore.signal();
  std::string error;
  auto nextRetry = std::chrono::steady_clock::now();
  for (;;) {
    int timeout = -1;
    {
      auto lock = lockEventHandling();
      if (!renameEvent.isStarted && inotifyService->mTree->needsEntryRetry()) {
        auto now = std::chrono::steady_clock::now();
        if (now >= nextRetry) {
          inotifyService->mTree->retryChangedEntry();
          // The symlinks that waited for the last of these entries, see InotifyTree::includeLinks().
          inotifyService->revalidateLinks();
          nextRetry = now + std::chrono::milliseconds(50);
        }
        timeout = 50;
      }
    }
    pollfd inputs[] = { { mInotifyInstance, POLLIN, 0 }, { mWakePipe[0], POLLIN, 0 } };
    int ready = poll(inputs, 2, timeout);
    if (ready == -1 && errno == EINTR) continue;
    if (ready == -1) {
      error = readErrorMessage(-1, errno);
      break;
    }
    if (inputs[1].revents & POLLIN) {
      char signals[64];
      ssize_t consumed = read(mWakePipe[0], signals, sizeof(signals));
      (void)consumed;
    }
    if (!inputs[0].revents) continue;
    ssize_t bytesRead = read(mInotifyInstance, &buffer, BUFFER_SIZE);
    if (bytesRead == -1 && errno == EINTR) {
      continue;
    }
    if (bytesRead <= 0) {
      error = readErrorMessage(bytesRead, errno);
      break;
    }

    #ifdef NSFW_TEST_SLOW_1
      pauseBeforeLock(buffer, bytesRead);
    #endif

    std::unique_lock<std::mutex> syncWithDestructor(mMutex);
    inotifyService->startRead(bytesRead);
    ssize_t position = 0;
    // What this read takes from the queue, less what removeDroppedWatch() puts back.
    int eventsRead = 0;
    do {
      event = (struct inotify_event *)(buffer + position);
      ++eventsRead;

      // The kernel queues this (with wd -1) once the queue holds fs.inotify.max_queued_events events, and drops the
      // events that follow, so the tree may be missing directories and the watcher can't be trusted anymore. Check it
      // first, so it never completes a pending rename or reaches a handler.
      if (event->mask & (uint32_t)IN_Q_OVERFLOW) {
        error = "Inotify event queue overflowed, events were lost";
        break;
      }

      #ifdef NSFW_TEST_SLOW_1
        runTestHook(event);
      #endif

      if (event->mask & (uint32_t)IN_IGNORED) {
        inotifyService->watchIgnored(event->wd);
      }

      // Events queued before the watch was found again cannot end a pending move.
      if (dropQuarantined()) {
        continue;
      }

      // IN_IGNORED, queued when a watch is removed, can come between the halves of a move and has no handler.
      if (renameEvent.isStarted && event->cookie != renameEvent.cookie && !(event->mask & (uint32_t)IN_IGNORED)) {
        renameAbandon();
      }

      if (!(event->mask & (uint32_t)IN_IGNORED) && inotifyService->removeDroppedWatch(event->wd)) {
        --eventsRead;
      }

      // An event dropped above, see dropQuarantined(), may have been the arrival of a directory at an excluded path:
      // the links that lead into it go before this event is reported at one of them, not only once the read is
      // handled. Not between the halves of a move, like the other maintenance.
      if (!renameEvent.isStarted) {
        inotifyService->revalidateLinks();
      }

      // Also when the steps above just found its directory again.
      if (dropQuarantined()) {
        continue;
      }

      isDirectoryRemoval = event->mask & (uint32_t)(IN_IGNORED | IN_DELETE_SELF);
      isDirectoryEvent = event->mask & (uint32_t)(IN_ISDIR);

      if (event->mask & (uint32_t)(IN_ATTRIB | IN_MODIFY)) {
        modify();
      } else if (event->mask & (uint32_t)IN_CREATE) {
        create(true);
        inotifyService->revalidateLinks();
      } else if (event->mask & (uint32_t)(IN_DELETE | IN_DELETE_SELF)) {
        remove();
        inotifyService->revalidateLinks();
      } else if (event->mask & (uint32_t)IN_MOVED_TO) {
        if (event->cookie == 0) {
          create(false);
        } else {
          renameEnd();
        }
        inotifyService->revalidateLinks();
      } else if (event->mask & (uint32_t)IN_MOVED_FROM) {
        if (event->cookie == 0) {
          remove();
          inotifyService->revalidateLinks(isDirectoryEvent);
        } else {
          renameStart();
        }
      } else if (event->mask & (uint32_t)IN_MOVE_SELF) {
        inotifyService->removeDirectory(event->wd);
      }
    } while((position += sizeof(struct inotify_event) + event->len) < bytesRead);
    if (!error.empty()) {
      // The run is about to end with this error and the events still queued are discarded, so a pending rename is
      // dropped instead of being reported as a removal.
      break;
    }
    inotifyService->finishRead(eventsRead, bytesRead);
    if (renameEvent.isStarted) {
      inotifyService->removeDroppedWatches(eventsRead);
      eventsRead = 0;
      // The read may have split the move in two, or taken the IN_MOVED_FROM before the kernel queued its IN_MOVED_TO,
      // so its other half gets a short wait, without holding up the threads that take the lock. The pending rename is
      // kept for the next read if events arrive (IN_IGNORED of removed watches included, which don't end it), or if
      // poll() fails, until the next event of another cookie.
      syncWithDestructor.unlock();
      pollfd pending = { mInotifyInstance, POLLIN, 0 };
      int ready;
      do {
        ready = poll(&pending, 1, RENAME_PAIRING_TIMEOUT_MS);
      } while (ready == -1 && errno == EINTR);
      if (ready == 0) {
        syncWithDestructor.lock();
        renameAbandon();
      }
    }
    if (!renameEvent.isStarted) {
      // Not with a move pending: its other half may put the directory a retry is for where it has a node, see
      // InotifyTree::retryWatches().
      inotifyService->mTree->resumeFound();
      inotifyService->revalidateRejected();
      inotifyService->revalidateLinks();
      inotifyService->removeDroppedWatches(eventsRead);
      inotifyService->retryWatches();
      // With nothing queued, see InotifyTree::forgetIncompleteScans().
      inotifyService->forgetIncompleteScans();
    }
  }

  // Errors are terminal: the poll thread reports this one (or the one recorded before it, or the root path error if the
  // watched path is gone, see InotifyTree::getError()) through errorCallback and stops the watcher. isStarted() stays
  // true, see its declaration.
  auto lock = lockEventHandling();
  inotifyService->setError(error);
  inotifyService->mTree->cancelFound();
}

#ifdef NSFW_TEST_SLOW_1
// Lets js/spec/index-slow-spec.js drive the failures it can't cause from outside, when it creates a file with one of
// these names in a watched directory.
void InotifyEventLoop::runTestHook(const inotify_event *event) {
  if (!(event->mask & (uint32_t)IN_CREATE) || event->len == 0) {
    return;
  }

  std::string name = event->name;
  if (name == "nsfw-test-hold-event-loop") {
    // Stops reading events until the release file shows up next to this one or the directory goes away (for at most a
    // minute), so the kernel queue overflows if enough events happen meanwhile.
    std::string directory;
    if (mInotifyService->mTree->getPath(directory, event->wd)) {
      waitForTestFile(directory, "nsfw-test-release-event-loop");
    }
  } else if (name == "nsfw-test-fail-read") {
    failReads(mInotifyInstance);
  }
}

// Holds a read that has the creation of nsfw-test-pause-before-lock, without the lock, until
// nsfw-test-resume-event-loop shows up next to it (for at most a minute): other threads can change the tree meanwhile,
// like updateExcludedPaths(), and the events they cause queue behind the ones read.
void InotifyEventLoop::pauseBeforeLock(const char *buffer, ssize_t bytesRead) {
  for (ssize_t position = 0; position < bytesRead;) {
    const inotify_event *event = (const inotify_event *)(buffer + position);
    position += sizeof(struct inotify_event) + event->len;
    if (
      !(event->mask & (uint32_t)IN_CREATE) || event->len == 0 ||
      std::string(event->name) != "nsfw-test-pause-before-lock"
    ) {
      continue;
    }
    std::string directory;
    {
      std::lock_guard<std::mutex> lock(mMutex);
      if (!mInotifyService->mTree->getPath(directory, event->wd)) {
        return;
      }
    }
    waitForTestFile(directory, "nsfw-test-resume-event-loop");
    return;
  }
}

// Waits until a file named release shows up in directory, or directory goes away, for at most a minute. Creates
// /tmp/nsfw-test-waiting-<pid> first, outside any watched path, so the spec knows the wait started.
void InotifyEventLoop::waitForTestFile(const std::string &directory, const std::string &release) {
  std::string waitingPath = "/tmp/nsfw-test-waiting-" + std::to_string(getpid());
  int waiting = open(waitingPath.c_str(), O_CREAT | O_WRONLY | O_NOFOLLOW, 0644);
  if (waiting != -1) {
    close(waiting);
  }
  std::string releasePath = directory + "/" + release;
  struct stat file;
  for (int i = 0; i < 600 && stat(releasePath.c_str(), &file) != 0 && stat(directory.c_str(), &file) == 0; ++i) {
    std::this_thread::sleep_for(std::chrono::milliseconds(100));
  }
}

// Puts a write-only descriptor in place of the inotify one, so the next read() fails with EBADF.
bool InotifyEventLoop::failReads(int inotifyInstance) {
  int writeOnly = open("/dev/null", O_WRONLY);
  if (writeOnly == -1) {
    return false;
  }
  dup2(writeOnly, inotifyInstance);
  close(writeOnly);
  return true;
}
#endif

InotifyEventLoop::~InotifyEventLoop() {
  if (mStarted) {
    {
      std::lock_guard<std::mutex> syncWithWork(mMutex);
      pthread_cancel(mEventLoop);
    }
    pthread_join(mEventLoop, NULL);
  }
  for (int fd : mWakePipe) {
    if (fd >= 0) close(fd);
  }
}
