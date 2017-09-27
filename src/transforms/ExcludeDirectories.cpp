#include "../../includes/transforms/ExcludeDirectories.h"
#include <regex>

ExcludeDirectories::ExcludeDirectories(const std::string &regex) : mRegex(regex) {
}

VecEvents ExcludeDirectories::transform(VecEvents vecEvents) {
  std::regex self_regex(mRegex,
                        std::regex_constants::ECMAScript
                        | std::regex_constants::icase);
  vecEvents->erase(std::remove_if(vecEvents->begin(),
                                  vecEvents->end(),
                                  [this, &self_regex](std::unique_ptr<Event> &event){
    return std::regex_search(event->directory, self_regex);
  }), vecEvents->end());

  return vecEvents;
}
