#include "../includes/osx/FSEventsService.h"
#include <iostream>
#include <unistd.h>

int main() {
  FSEventsService run("/Users/tylerw/github/nsfw/a");

  while(true) {
    sleep(1);
  }

  return 0;
}
