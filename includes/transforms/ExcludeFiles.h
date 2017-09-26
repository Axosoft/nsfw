#ifndef NSFW_TRANSFORMS_EXCLUDEFILESH
#define NSFW_TRANSFORMS_EXCLUDEFILESH

#include <regex>

#include "AbstractTransform.h"

class ExcludeFiles : public AbstractTransform
{
public:
  ExcludeFiles(const std::string &regex);

  VecEvents transform(VecEvents vecEvents);

private:
  std::string mRegex;
};

#endif
