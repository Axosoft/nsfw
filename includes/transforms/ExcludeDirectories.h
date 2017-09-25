#ifndef NSFW_TRANSFORMS_EXCLUDEDIRECTORIES_H
#define NSFW_TRANSFORMS_EXCLUDEDIRECTORIES_H

#include "AbstractTransform.h"

#include <regex>

class ExcludeDirectories : public AbstractTransform
{
public:
  ExcludeDirectories(const std::string &regex)
    : regex(regex) {}

  VecEvents transform(VecEvents vecEvents) {
    vecEvents->erase(std::remove_if(vecEvents->begin(),
                                 vecEvents->end(),
                                 [this](std::unique_ptr<Event> &event){
      std::regex self_regex(regex,
                            std::regex_constants::ECMAScript
                          | std::regex_constants::icase);
      return std::regex_search(event->directory, self_regex);
    }), vecEvents->end());

    return vecEvents;
  }

  virtual ~ExcludeDirectories() {}

private:
  std::string regex;
};

#endif
