#ifndef NSFW_QUEUE_H
#define NSFW_QUEUE_H

#include <string>
#include <queue>
#include <iostream>

enum EventType {
  CREATED = 0,
  DELETED = 1,
  MODIFIED = 2,
  RENAMED = 3
};

class Queue {
public:
  void enqueue(
    EventType action,
    std::string directory,
    std::string name,
    std::string
    renamedName = ""
  );
};

#endif
