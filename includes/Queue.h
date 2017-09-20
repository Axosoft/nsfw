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
  Event(const EventType type, const std::string& directory, const std::string& fileA, const std::string& fileB = "") :
      type(type), directory(directory), fileA(fileA), fileB(fileB)
  {
    timePoint = std::chrono::high_resolution_clock::now();
  }
  EventType type;
  std::string directory, fileA, fileB;
  std::chrono::high_resolution_clock::time_point timePoint;
};

class EventQueue {
public:
  void clear();
  std::size_t count();
  std::unique_ptr<Event> dequeue();
  std::unique_ptr<std::vector<Event*>> dequeueAll();
  std::unique_ptr<std::vector<std::unique_ptr<Event>>> dequeueAllEventsInVector();

  void enqueue(
    EventType type,
    const std::string& directory,
    const std::string& fileA,
    const std::string& fileB = ""
  );

private:
  std::deque<std::unique_ptr<Event>> queue;
  std::mutex mutex;
};

#endif
