#include "../../includes/linux/InotifyEventLoop.h"

#include <climits>
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
  wake();
}

void InotifyEventLoop::wake() {
  ssize_t sent;
  do {
    sent = write(mWakePipe[1], "1", 1);
  } while (sent == -1 && errno == EINTR);
  if (sent == -1 && errno != EAGAIN) mInotifyService->setError("Cannot wake inotify reader");
}

// A move whose IN_MOVED_TO didn't come: the entry most likely went somewhere that isn't watched (outside the watched
// path, or an excluded directory). An IN_MOVED_TO that comes after abandonment is reported as a creation.
// The event that triggers abandonment may be in another buffer.
void InotifyEventLoop::abandonRename() {
  if (!mRename.isStarted) {
    return;
  }

  mRename.isStarted = false;
  if (mRename.isDirectory) {
    mInotifyService->removeMovedDirectory(mRename.wd, mRename.name);
  } else {
    mInotifyService->remove(mRename.wd, mRename.name, false);
    mInotifyService->revalidateLinks();
  }
}

// Reads the next events into buffer, with event handling locked, so they are handled in the order they were queued
// whichever thread reads them, see drain(). Their size, 0 if none are queued, -1 when the run ends, with error set.
ssize_t InotifyEventLoop::readBuffer(char *buffer, std::string &error) {
  ssize_t bytesRead;
  do {
    bytesRead = read(mInotifyInstance, buffer, BUFFER_SIZE);
  } while (bytesRead == -1 && errno == EINTR);
  if (bytesRead == -1 && (errno == EAGAIN || errno == EWOULDBLOCK)) {
    return 0;
  }
  if (bytesRead <= 0) {
    error = readErrorMessage(bytesRead, errno);
    return -1;
  }
  return bytesRead;
}

// Handles a read, with event handling locked. eventsRead: what it takes from the queue, less what removeDroppedWatch()
// puts back. False when the run ends, with error set unless the tree errored or the queue overflowed.
bool InotifyEventLoop::handleBuffer(char *buffer, ssize_t bytesRead, int &eventsRead, std::string &error) {
  inotify_event *event = NULL;
  bool isDirectoryEvent = false, isDirectoryRemoval = false;
  InotifyService *inotifyService = mInotifyService;

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

    inotifyService->modify(event->wd, event->len > 0 ? event->name : "", isDirectoryEvent);
  };

  auto remove = [&event, &isDirectoryRemoval, &isDirectoryEvent, &inotifyService]() {
    if (event == NULL) {
      return;
    }

    if (isDirectoryRemoval) {
      inotifyService->removeDirectory(event->wd);
    } else {
      inotifyService->remove(event->wd, event->len > 0 ? event->name : "", isDirectoryEvent);
    }
  };

  auto renameStart = [&event, &isDirectoryEvent, this]() {
    mRename.cookie = event->cookie;
    mRename.isDirectory = isDirectoryEvent;
    mRename.name = event->len > 0 ? event->name : "";
    mRename.wd = event->wd;
    mRename.isStarted = true;
    mRename.giveUpAt = std::chrono::steady_clock::now() + std::chrono::milliseconds(RENAME_PAIRING_TIMEOUT_MS);
  };

  auto renameEnd = [&create, &event, &inotifyService, this]() {
    if (!mRename.isStarted) {
      // From a directory that isn't watched.
      create(false);
      return;
    }

    mRename.isStarted = false;
    if (mRename.isDirectory) {
      inotifyService->renameDirectory(mRename.wd, mRename.name, event->wd, event->len > 0 ? event->name : "");
    } else {
      inotifyService->rename(mRename.wd, mRename.name, event->wd, event->len > 0 ? event->name : "");
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

  inotifyService->startRead(bytesRead);
  ssize_t position = 0;
  eventsRead = 0;
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
    // So did the queue JavaScript takes events from: the delivery thread reports it, and the run ends without the
    // watches and scans the rest of this read would take.
    if (inotifyService->mQueue->overflowed()) break;

    bool hasRenameCookie = event->cookie != 0;
    #ifdef NSFW_FREEBSD_SYSTEM_INOTIFY
      // The kernel's rename cookie counter starts at zero.
      hasRenameCookie = true;
    #endif
    #ifdef NSFW_TEST_SLOW_1
      if ((event->mask & IN_MOVED_FROM) && event->len > 0 &&
          strcmp(event->name, "nsfw-test-zero-move-cookie") == 0) {
        event->cookie = 0;
        hasRenameCookie = true;
      }
      runTestHook(event);
    #endif

    if (event->mask & (uint32_t)IN_IGNORED) {
      inotifyService->watchIgnored(event->wd);
    }

    // Events queued before the watch was found again cannot end a pending move.
    if (dropQuarantined()) {
      continue;
    }

    // Ordinary FreeBSD events also have cookie zero. IN_IGNORED can come between the two halves because watch
    // removal queues it separately.
    if (mRename.isStarted && !(event->mask & (uint32_t)IN_IGNORED) &&
        (!(event->mask & (uint32_t)IN_MOVED_TO) || event->cookie != mRename.cookie)) {
      abandonRename();
    }

    if (!(event->mask & (uint32_t)IN_IGNORED) && inotifyService->removeDroppedWatch(event->wd)) {
      --eventsRead;
    }

    // An event dropped above, see dropQuarantined(), may have been the arrival of a directory at an excluded path:
    // the links that lead into it go before this event is reported at one of them, not only once the read is
    // handled. Not between the halves of a move, like the other maintenance.
    if (!mRename.isStarted) {
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
      if (!hasRenameCookie) {
        create(false);
      } else {
        renameEnd();
      }
      inotifyService->revalidateLinks();
    } else if (event->mask & (uint32_t)IN_MOVED_FROM) {
      if (!hasRenameCookie) {
        remove();
        inotifyService->revalidateLinks(isDirectoryEvent);
      } else {
        renameStart();
      }
    } else if (event->mask & (uint32_t)IN_MOVE_SELF) {
      inotifyService->removeDirectory(event->wd);
    }
  } while((position += sizeof(struct inotify_event) + event->len) < bytesRead);
  if (!error.empty() || inotifyService->mQueue->overflowed()) {
    // The run is about to end with this error, or the queue's overflow, and the events still queued are discarded,
    // so a pending rename is dropped instead of being reported as a removal.
    return false;
  }
  inotifyService->finishRead(eventsRead, bytesRead);
  return true;
}

// Once a read is handled. With a move pending, only what the read lets go of: its other half may put the directory a
// retry is for where it has a node, see InotifyTree::retryWatches().
void InotifyEventLoop::finishBatch(int eventsRead) {
  if (mRename.isStarted) {
    mInotifyService->removeDroppedWatches(eventsRead);
    return;
  }
  mInotifyService->mTree->resumeFound();
  mInotifyService->revalidateRejected();
  mInotifyService->revalidateLinks();
  mInotifyService->removeDroppedWatches(eventsRead);
  mInotifyService->retryWatches();
  // With nothing queued, see InotifyTree::forgetIncompleteScans().
  mInotifyService->forgetIncompleteScans();
}

// poll()'s result: 0 once the timeout passes with nothing queued.
int InotifyEventLoop::waitForEvents(int timeout) {
  pollfd pending = { mInotifyInstance, POLLIN, 0 };
  int ready;
  do {
    ready = poll(&pending, 1, timeout);
  } while (ready == -1 && errno == EINTR);
  return ready;
}

// Ends the run, with error, if any, whichever thread finds it: the loop stops at its next turn.
void InotifyEventLoop::endRun(const std::string &error) {
  if (!error.empty()) mInotifyService->setError(error);
  mEnded = true;
  wake();
}

// Reads and handles the next events, with event handling locked: their size, 0 if none are queued, -1 when the run
// ended. loopLock: the loop's, which a test hook lets go of meanwhile, see pauseAfterRead().
ssize_t InotifyEventLoop::readEvents(char *buffer, std::unique_lock<std::mutex> *loopLock) {
  std::string error;
  ssize_t bytesRead = readBuffer(buffer, error);
  if (bytesRead == 0) {
    return 0;
  }
  #ifdef NSFW_TEST_SLOW_1
    if (bytesRead > 0 && loopLock != nullptr) {
      pauseAfterRead(*loopLock, buffer, bytesRead);
      if (mEnded) return -1;
    }
  #else
    (void)loopLock;
  #endif
  int eventsRead = 0;
  if (bytesRead < 0 || !handleBuffer(buffer, bytesRead, eventsRead, error)) {
    // Before the lock is let go: no thread reads past the end of the run.
    endRun(error);
    return -1;
  }
  finishBatch(eventsRead);
  return bytesRead;
}

// How long the pending move may still wait for its other half, in milliseconds.
int InotifyEventLoop::renameWait() {
  auto left = mRename.giveUpAt - std::chrono::steady_clock::now();
  return left.count() <= 0 ? 0 : (int)std::chrono::ceil<std::chrono::milliseconds>(left).count();
}

// A move whose other half didn't come in time, see abandonRename(). Whichever thread finds it so, see work().
void InotifyEventLoop::giveUpOnExpiredRename() {
  if (mRename.isStarted && std::chrono::steady_clock::now() >= mRename.giveUpAt) {
    abandonRename();
    finishBatch(0);
  }
}

bool InotifyEventLoop::drain() {
  if (!mStarted || mEnded) {
    return false;
  }
  // Not under a path that leads elsewhere now: the run ends with the error hasErrored() records, and the events queued
  // with it, see work().
  if (mInotifyService->mTree->hasErrored()) {
    endRun("");
    return false;
  }
  char buffer[BUFFER_SIZE];
  // Only what is queued now: changes can keep coming for as long as this reads. A bounded part of it where the size
  // can't be told.
  const int queued = mInotifyService->mTree->getQueuedBytes();
  const int64_t bound = queued == INT_MAX ? (int64_t)BUFFER_SIZE * UNKNOWN_QUEUE_DRAIN_READS : queued;
  for (int64_t taken = 0; taken < bound;) {
    ssize_t bytesRead = readEvents(buffer);
    if (bytesRead < 0) return false;
    if (bytesRead == 0) break;
    taken += bytesRead;
  }
  // The other half of a move pending at the end, which the kernel queues along with it, or else what is left of its
  // wait, see work().
  if (mRename.isStarted) {
    if (waitForEvents(renameWait()) > 0) {
      if (readEvents(buffer) < 0) return false;
    } else {
      giveUpOnExpiredRename();
    }
    // One still waiting, which that read may have started, is the loop's.
    if (mRename.isStarted) wake();
  }
  return true;
}

void InotifyEventLoop::work() {
  char buffer[BUFFER_SIZE];
  InotifyService *inotifyService = mInotifyService;

  mLoopingSemaphore.signal();
  std::string error;
  auto nextRetry = std::chrono::steady_clock::now();
  for (;;) {
    // The run ends with the overflow, whichever thread queued the event that caused it: nothing more to watch for it.
    if (inotifyService->mQueue->overflowed()) break;
    int timeout = -1;
    // A read may have split a move in two, or taken the IN_MOVED_FROM before the kernel queued its IN_MOVED_TO, so its
    // other half gets a short wait, see renameWait(), without holding up the threads that take the lock. A drain
    // meanwhile may read it, see drain().
    bool renamePending;
    {
      auto lock = lockEventHandling();
      if (mEnded) break;
      renamePending = mRename.isStarted;
      if (renamePending) {
        timeout = renameWait();
      } else {
        // An ignore file change left for later, see InotifyService::settleFilterChange().
        int filterWait = inotifyService->settleFilterChange();
        if (filterWait >= 0) {
          timeout = filterWait;
        }
      }
      if (!mRename.isStarted && inotifyService->mTree->needsEntryRetry()) {
        auto now = std::chrono::steady_clock::now();
        if (now >= nextRetry) {
          inotifyService->mTree->retryChangedEntry();
          // The symlinks that waited for the last of these entries, see InotifyTree::includeLinks().
          inotifyService->revalidateLinks();
          nextRetry = now + std::chrono::milliseconds(50);
        }
        timeout = timeout < 0 ? 50 : std::min(timeout, 50);
      }
    }
    // Idle, the watched path can still come to lead elsewhere with no event in the tree (a directory above it renamed),
    // so it is checked each second. On whole seconds of the monotonic clock, so every watcher wakes at the same time.
    auto beforePoll = std::chrono::steady_clock::now();
    auto rootCheckAt = std::chrono::time_point_cast<std::chrono::seconds>(beforePoll) + std::chrono::seconds(1);
    int untilRootCheck = (int)std::chrono::ceil<std::chrono::milliseconds>(rootCheckAt - beforePoll).count();
    timeout = timeout < 0 ? untilRootCheck : std::min(timeout, untilRootCheck);
    pollfd inputs[] = { { mInotifyInstance, POLLIN, 0 }, { mWakePipe[0], POLLIN, 0 } };
    int ready = poll(inputs, 2, timeout);
    if (ready == -1 && errno == EINTR) continue;
    if (ready == -1) {
      error = readErrorMessage(-1, errno);
      // So a drain doesn't read past the end of the run, see drain().
      auto lock = lockEventHandling();
      mEnded = true;
      break;
    }
    if (inputs[1].revents & POLLIN) {
      char signals[64];
      ssize_t consumed = read(mWakePipe[0], signals, sizeof(signals));
      (void)consumed;
    }
    if (!inputs[0].revents) {
      if (renamePending) {
        auto lock = lockEventHandling();
        if (mEnded) break;
        giveUpOnExpiredRename();
      }
      if (std::chrono::steady_clock::now() >= rootCheckAt && inotifyService->mTree->hasErrored()) break;
      continue;
    }
    // Not under a path that leads elsewhere now: the run ends with the error hasErrored() records, and the events
    // queued with it. Before the lock, as hasErrored() may stat a slow filesystem.
    if (inotifyService->mTree->hasErrored()) break;

    std::unique_lock<std::mutex> syncWithDestructor(mMutex);
    if (mEnded) break;
    // None when a drain took what poll() saw, see drain().
    if (readEvents(buffer, &syncWithDestructor) < 0) break;
  }

  // Errors are terminal: the delivery thread reports this one (or the one recorded before it, or the root path error if
  // the watched path is gone, see InotifyTree::getError(), or the queue's overflow, which records none here) through
  // errorCallback and stops the watcher. isStarted() stays true, see its declaration.
  auto lock = lockEventHandling();
  mEnded = true;
  if (!error.empty()) inotifyService->setError(error);
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

// Holds a read that has the creation of nsfw-test-pause-after-read, with the lock released, until
// nsfw-test-resume-event-loop shows up next to it (for at most a minute): other threads can change the tree meanwhile,
// like updateExcludedPaths(), and the events they cause queue behind the ones read. A drain meanwhile, see
// updateExcludedPatterns(), handles the events queued after the read held here before that read: only tests do this.
void InotifyEventLoop::pauseAfterRead(std::unique_lock<std::mutex> &lock, const char *buffer, ssize_t bytesRead) {
  for (ssize_t position = 0; position < bytesRead;) {
    const inotify_event *event = (const inotify_event *)(buffer + position);
    position += sizeof(struct inotify_event) + event->len;
    if (
      !(event->mask & (uint32_t)IN_CREATE) || event->len == 0 ||
      std::string(event->name) != "nsfw-test-pause-after-read"
    ) {
      continue;
    }
    std::string directory;
    if (!mInotifyService->mTree->getPath(directory, event->wd)) {
      return;
    }
    lock.unlock();
    waitForTestFile(directory, "nsfw-test-resume-event-loop");
    lock.lock();
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
