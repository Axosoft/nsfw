#ifndef LOCK_H
#define LOCK_H

#include <pthread.h>

class Lock {
public:
  Lock(pthread_mutex_t &mutex);
  ~Lock();
private:
  pthread_mutex_t &mMutex;
};

#endif
