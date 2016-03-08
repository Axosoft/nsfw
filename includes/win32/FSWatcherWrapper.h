#include "FSWatcherSingleton.h"

class FSWatcherWrapper {
public:
  FSWatcherWrapper(std::string path);

  bool isWatching();

  ~FSWatcherWrapper();
private:
  int mWD;
};
