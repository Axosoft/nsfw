#ifndef NSFW_TRANSFORMS_EXCLUDEFILESH
#define NSFW_TRANSFORMS_EXCLUDEFILESH

#include <regex>

#include "AbstractTransform.h"

class ExcludeFiles : public AbstractTransform
{
public:
    ExcludeFiles(const std::string &regex)
        : regexStr(regex) {

    }

    VecEvents transform(VecEvents vecEvents)
    {
        vecEvents->erase(std::remove_if(vecEvents->begin(),
                                       vecEvents->end(),
                                       [this](std::unique_ptr<Event> &event){
            std::regex self_regex(regexStr,
                                  std::regex_constants::ECMAScript
                                | std::regex_constants::icase);
            return std::regex_search(event->fileA, self_regex);
        }), vecEvents->end());

        return vecEvents;
    }
private:
    std::string regexStr;
};

#endif
