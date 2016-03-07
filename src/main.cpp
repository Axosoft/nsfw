#include "../includes/NativeInterface.h"
#include <stdlib.h>
#include <unistd.h>

int main() {
  char path[PATH_MAX + 1];
  realpath("./a", path);
  NativeInterface service(path);
  while(true) {
    sleep(1);
  }
  return 0;
}
