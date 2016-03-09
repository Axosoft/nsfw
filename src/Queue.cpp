#include "../includes/Queue.h"

#pragma unmanaged
EventQueue::EventQueue() {
  OPA_Queue_init(&mQueue);
  OPA_store_int(&mNumEvents, 0);
}

EventQueue::~EventQueue() {
  while(!OPA_Queue_is_empty(&mQueue)) {
    EventNode *node;

    OPA_Queue_dequeue(&mQueue, node, EventNode, header);

    delete node->event;
    delete node;
  }
}

void EventQueue::clear() {
  while(!OPA_Queue_is_empty(&mQueue)) {
    EventNode *node;

    OPA_decr_int(&mNumEvents);
    OPA_Queue_dequeue(&mQueue, node, EventNode, header);

    delete node->event;
    delete node;
  }
}

int EventQueue::count() {
  return OPA_load_int(&mNumEvents);
  return 0;
}

Event *EventQueue::dequeue() {
  if (!OPA_Queue_is_empty(&mQueue)) {
    EventNode *node;

    OPA_decr_int(&mNumEvents);
    OPA_Queue_dequeue(&mQueue, node, EventNode, header);

    Event *event = node->event;
    delete node;

    return event;
  }
  return NULL;
}

void EventQueue::enqueue(EventType type, std::string directory, std::string fileA, std::string fileB) {
  EventNode *node = new EventNode;

  OPA_Queue_header_init(&node->header);

  node->event = new Event;
  node->event->type = type;
  node->event->directory = directory;
  node->event->fileA = fileA;
  node->event->fileB = fileB;

  OPA_Queue_enqueue(&mQueue, node, EventNode, header);
  OPA_incr_int(&mNumEvents);
}
