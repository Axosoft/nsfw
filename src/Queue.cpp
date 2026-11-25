#include "../includes/Queue.h"
#include <iostream>

#pragma unmanaged

void EventQueue::clear() {
  std::lock_guard<std::mutex> lock(mutex);
  queue.clear();
  mHeldFrom = 0;
  ++mEpoch;
}

void EventQueue::holdEvents() {
  std::lock_guard<std::mutex> lock(mutex);
  if (mHolding) return;
  mHolding = true;
  mHeldFrom = queue.size();
}

bool EventQueue::releaseEvents(bool keepTrailingRemoval) {
  bool keptRemoval;
  bool released;
  {
    std::lock_guard<std::mutex> lock(mutex);
    if (!mHolding) return false;
    keptRemoval = keepTrailingRemoval && queue.size() > mHeldFrom && queue.back()->type == DELETED;
    const std::size_t deliverable = queue.size() - (keptRemoval ? 1 : 0);
    released = deliverable > mHeldFrom;
    mHolding = keptRemoval;
    mHeldFrom = keptRemoval ? deliverable : 0;
  }
  if (released) wake(WAKE_EVENTS);
  return keptRemoval;
}

std::size_t EventQueue::count() {
  std::lock_guard<std::mutex> lock(mutex);
  return queue.size();
}

std::unique_ptr<Event> EventQueue::dequeue() {
  std::lock_guard<std::mutex> lock(mutex);
  if (queue.empty() || (mHolding && mHeldFrom == 0)) {
    return nullptr;
  }
  if (mHolding) --mHeldFrom;

  auto &front = queue.front();
  auto retVal = std::move(front);
  queue.pop_front();

  return retVal;
}

std::unique_ptr<std::vector<std::unique_ptr<Event>>> EventQueue::dequeueAll(uint64_t &epoch) {
  std::lock_guard<std::mutex> lock(mutex);
  const auto queueSize = mHolding ? mHeldFrom : queue.size();
  if (queueSize == 0) {
    return nullptr;
  }

  std::unique_ptr<std::vector<std::unique_ptr<Event>>> events(new std::vector<std::unique_ptr<Event>>);
  for (size_t i = 0; i < queueSize; ++i) {
    auto &front = queue.front();
    events->emplace_back(std::move(front));
    queue.pop_front();
  }
  mHeldFrom = 0;
  epoch = mEpoch;

  return events;
}

void EventQueue::enqueue(
  const EventType type,
  const std::string &fromDirectory,
  const std::string &fromFile,
  const std::string &toDirectory,
  const std::string &toFile,
  PathFilter::Kind kind
) {
  const auto eventEpoch = epoch();
  std::shared_ptr<PathFilter> filter;
  bool watcherNotesEntries;
  {
    std::lock_guard<std::mutex> lock(mFilterMutex);
    filter = mFilter;
    watcherNotesEntries = mWatcherNotesEntries;
  }
  if (!filter || !filter->isActive()) {
    push(eventEpoch, type, fromDirectory, fromFile, toDirectory, toFile);
    return;
  }

  // Also while paused: the rules have to follow their files whether or not their changes are reported. On macOS a
  // modification counts as a replacement too: it reports a directory that moved in as modified when it can't tell,
  // see FSEventsService::demangle(). Elsewhere it doesn't, so Windows reporting a directory as modified whenever an
  // entry changes in it doesn't have its rules read again each time.
  if (!watcherNotesEntries) {
#if defined(__APPLE__)
    const bool replaced = true;
#else
    const bool replaced = type != MODIFIED;
#endif
    if (type == RENAMED) {
      filter->entryMoved(fromDirectory, fromFile, toDirectory, toFile);
    } else {
      filter->entryChanged(fromDirectory, fromFile, replaced);
    }
  }
  if (mPaused) {
    return;
  }

  if (type == DELETED) {
    if (!filter->isRemovalIgnored(fromDirectory, fromFile, kind)) push(eventEpoch, type, fromDirectory, fromFile);
    return;
  }
  if (type != RENAMED) {
    bool replacedReported = false;
    if (!filter->isReplacementIgnored(fromDirectory, fromFile, kind, replacedReported)) {
      push(eventEpoch, type, fromDirectory, fromFile);
    } else if (replacedReported) {
      push(eventEpoch, DELETED, fromDirectory, fromFile);
    }
    return;
  }
  // What left is what arrived, which can still be looked up.
  if (kind == PathFilter::Kind::UNKNOWN) {
    kind = PathFilter::kindOf(toDirectory, toFile);
  }
  const bool fromIgnored = filter->isRemovalIgnored(fromDirectory, fromFile, kind);
  bool replacedReported = false;
  const bool toIgnored = filter->isReplacementIgnored(toDirectory, toFile, kind, replacedReported);
  if (toIgnored && replacedReported) {
    push(eventEpoch, DELETED, toDirectory, toFile);
  }
  if (!fromIgnored && !toIgnored) {
    push(eventEpoch, RENAMED, fromDirectory, fromFile, toDirectory, toFile);
  } else if (!toIgnored) {
    push(eventEpoch, CREATED, toDirectory, toFile);
  } else if (!fromIgnored) {
    push(eventEpoch, DELETED, fromDirectory, fromFile);
  }
}

void EventQueue::push(
  uint64_t epoch,
  const EventType type,
  const std::string &fromDirectory,
  const std::string &fromFile,
  const std::string &toDirectory,
  const std::string &toFile
) {
  bool held;
  {
    std::lock_guard<std::mutex> lock(mutex);
    if (mPaused || epoch != mEpoch) return;
    queue.emplace_back(std::unique_ptr<Event>(new Event(type, fromDirectory, fromFile, toDirectory, toFile)));
    held = mHolding;
  }
  if (!held) wake(WAKE_EVENTS);
}

void EventQueue::wake(unsigned reasons) {
  bool waiting;
  {
    std::lock_guard<std::mutex> lock(mWakeMutex);
    const unsigned added = reasons & ~mWakes;
    mWakes |= reasons;
    // A burst of events notifies once, and none while the delivery thread waits out the debounce for other reasons.
    waiting = (added & mWaitingFor) != 0;
  }
  if (waiting) mWakeCondition.notify_one();
}

unsigned EventQueue::waitForWake(unsigned reasons, std::chrono::steady_clock::time_point deadline) {
  std::unique_lock<std::mutex> lock(mWakeMutex);
  mWaitingFor = reasons;
  auto woken = [this, reasons]() { return (mWakes & reasons) != 0; };
  if (deadline == std::chrono::steady_clock::time_point::max()) {
    mWakeCondition.wait(lock, woken);
  } else {
    mWakeCondition.wait_until(lock, deadline, woken);
  }
  mWaitingFor = 0;
  const unsigned taken = mWakes & reasons;
  mWakes &= ~taken;
  return taken;
}

void EventQueue::resetWakes() {
  std::lock_guard<std::mutex> lock(mWakeMutex);
  mWakes = 0;
}

void EventQueue::setFilter(std::shared_ptr<PathFilter> filter, bool watcherNotesEntries) {
  std::lock_guard<std::mutex> lock(mFilterMutex);
  mFilter = std::move(filter);
  mWatcherNotesEntries = watcherNotesEntries;
}

void EventQueue::pause() {
  mPaused = true;
  clear();
}

void EventQueue::resume() {
  {
    // What entries were when last seen went stale while their events weren't judged.
    std::lock_guard<std::mutex> lock(mFilterMutex);
    if (mFilter) mFilter->forgetSeenKinds();
  }
  mPaused = false;
}

EventQueue::EventQueue():
  mPaused(false)
{}
