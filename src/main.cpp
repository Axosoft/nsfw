#include "../includes/linux/InotifyService.h"
#include <unistd.h>

int main() {
  InotifyService service("./a");
  while(true) {
    sleep(0);
  }
  return 0;
}
