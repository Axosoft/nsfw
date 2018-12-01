#ifndef NSFW_QUEUE_H
#define NSFW_QUEUE_H

#include <string>
#include <memory>
#include <deque>
#include <vector>
#include <mutex>

enum EventType {
  CREATED = 0,
  DELETED = 1,
  MODIFIED = 2,
  RENAMED = 3
};

struct Event {
  Event(const EventType type, const std::string &directory, const std::string &fileA, const std::string &fileB) :
      type(type), directory(directory), fileA(fileA), fileB(fileB) {}
  Event(const EventType type, const std::string &oldDirectory, const std::string &newDirectory, const std::string &fileA, const std::string &fileB) :
      type(type), directory(oldDirectory), newDirectory(newDirectory), fileA(fileA), fileB(fileB) {}
  EventType type;
  std::string directory, newDirectory, fileA, fileB;
};

class EventQueue {
public:
  void clear();
  std::size_t count();
  std::unique_ptr<Event> dequeue();
  std::unique_ptr<std::vector<Event*>> dequeueAll();
  void enqueue(
    EventType type,
    const std::string &directory,
    const std::string &fileA,
    const std::string &fileB = ""
  );
  // for renames
  void enqueue(
    EventType type,
    const std::string &oldDirectory,
    const std::string &newDirectory,
    const std::string &fileA,
    const std::string &fileB
  );

private:
  std::deque<std::unique_ptr<Event>> queue;
  std::mutex mutex;
};

#endif
