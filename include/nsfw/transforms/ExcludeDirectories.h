#ifndef NSFW_TRANSFORMS_EXCLUDEDIRECTORIES_H
#define NSFW_TRANSFORMS_EXCLUDEDIRECTORIES_H

#include "AbstractTransform.h"

class ExcludeDirectories : public AbstractTransform
{
public:
  ExcludeDirectories(const std::string &regex);

  VecEvents transform(VecEvents vecEvents);

private:
  std::string mRegex;
};

#endif
