#include "../includes/EventQueue.h"

namespace NSFW {
  EventQueue::EventQueue() {
    OPA_Queue_init(&mQueue);
    mNumEvents = 0;
  }

  EventQueue::~EventQueue() {
    while(!OPA_Queue_is_empty(&mQueue)) {
      EventNode *node;

      OPA_Queue_dequeue(&mQueue, node, EventNode, header);

      delete node->event;
      delete node;
    }
  }

  void EventQueue::enqueue(Action action, std::string directory, std::string fileA, std::string fileB) {
    EventNode *node = new EventNode;

    OPA_Queue_header_init(node->header);

    node->event = new Event;
    node->event->action = action;
    node->event->directory = directory;
    node->event->fileA = fileA;
    node->event->fileB = fileB;

    OPA_Queue_enqueue(&mQueue, node, EventNode, header);
    OPA_incr_int(&mNumEvents);
  }

  Event *EventQueue::dequeue() {
    if (!OPA_Queue_is_empty(&mQueue)) {
      EventNode *node;

      OPA_Queue_dequeue(&mQueue, node, EventNode, header);
      OPA_decr_int(&mNumEvents);

      Event *event = node->event;
      delete node;

      return event;
    }
    return NULL;
  }
}
