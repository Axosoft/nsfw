#include "../includes/Queue.h"

void writeLog(std::string type, std::string directory, std::string name) {
  std::cout
    << type
    << ": "
    << directory
    << "/"
    << name
    << std::endl;
}

void Queue::enqueue(EventType action, std::string directory, std::string name, std::string renamedName) {
  switch(action) {
  case CREATED:
    writeLog("CREATED", directory, name);
    break;
  case DELETED:
    writeLog("DELETED", directory, name);
    break;
  case MODIFIED:
    writeLog("MODIFIED", directory, name);
    break;
  case RENAMED:
    writeLog("RENAME A", directory, name);
    writeLog("RENAME B", directory, renamedName);
    break;
  }
}
