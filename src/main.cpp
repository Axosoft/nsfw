#include "../includes/win32/FSWatcherWrapper.h"
#include <windows.h>
#include <iostream>
#pragma unmanaged
int main() {
  while (true) {
    FSWatcherWrapper wrapper("c:\\github\\a");
    for (int i = 0; i < 15; ++i) {
      Sleep(1000);
    }
  }
  return 0;
}
