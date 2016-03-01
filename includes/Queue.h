#ifndef NSFW_QUEUE_H
#define NSFW_QUEUE_H

#include <string>
#include <queue>
#include <iostream>
#include "Types.h"

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
