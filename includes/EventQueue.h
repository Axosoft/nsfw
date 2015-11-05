extern "C" {
  #include <opa_queue.h>
}

namespace NSFW {

  enum Action { CREATED, DELETED, MODIFIED, RENAMED };

  struct EventNode {
    OPA_Queue_element_hdr_t header;
    Event *event;
  }

  struct Event {
    Action action;
    std::string directory, fileA, fileB;
  };

  class EventQueue {
  public:
    EventQueue();
    ~EventQueue();

    void enqueue(
      Action action,
      std::string directory,
      std::string fileA,
      std::string fileB = ""
    );

    // The event pointer must be freed.
    Event *dequeue();

  private:
    OPA_Queue_info_t mQueue;
    int mNumEvents;
  };
}
