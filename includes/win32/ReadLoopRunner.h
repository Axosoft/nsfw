#ifndef READ_LOOP_RUNNER_H
#define READ_LOOP_RUNNER_H

#include <windows.h>
#include <memory>
#include <sstream>
#include "../Queue.h"

// limited by over-the-network file watching
#define BUFFER_KB 1024

class ReadLoopRunner {
public:
  ReadLoopRunner(std::wstring directory, EventQueue &queue, HANDLE directoryHandle);
  ~ReadLoopRunner();

  static VOID CALLBACK eventCallback(DWORD errorCode, DWORD numBytes, LPOVERLAPPED overlapped);
  std::string getError();
  std::string getUTF8Directory(std::wstring path);
  void handleEvents();
  bool hasErrored();
  BOOL read();
  void resizeBuffers(unsigned int bufferSize);
  void setError(std::string error);
  void setSharedPointer(std::shared_ptr<ReadLoopRunner> *pointer);
  void swap(DWORD numBytes);
  void prepareForShutdown();

private:
  unsigned int mBufferSize;
  BYTE *mBuffer;
  std::wstring mDirectory;
  HANDLE mDirectoryHandle;
  std::string mErrorMessage;
  OVERLAPPED mOverlapped;
  BYTE *mSwap;
  EventQueue &mQueue;
};

#endif
