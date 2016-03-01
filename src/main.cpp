#include "../includes/NativeInterface.h"
#include <unistd.h>

int main() {
  NativeInterface service("./a");
  while(true) {
    sleep(0);
  }
  return 0;
}
