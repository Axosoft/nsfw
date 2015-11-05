extern "C" {
#  include <opa_queue.h>
#  include <opa_primitives.h>
}
#include <string>

namespace NSFW {

  enum Action { CREATED, DELETED, MODIFIED, RENAMED };

  struct Event {
    Action action;
    std::string directory, fileA, fileB;
  };

  struct EventNode {
    OPA_Queue_element_hdr_t header;
    Event *event;
  };

  class EventQueue {
  public:
    EventQueue();
    ~EventQueue();

    void clear();

    int count();

    // The event pointer must be freed.
    Event *dequeue();

    void enqueue(
      Action action,
      std::string directory,
      std::string fileA,
      std::string fileB = ""
    );

  private:
    OPA_Queue_info_t mQueue;
    OPA_int_t mNumEvents;
  };
}
