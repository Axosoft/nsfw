#ifndef READ_LOOP_H
#define READ_LOOP_H

#include <WinSock2.h>
#include <windows.h>
#include "ReadLoopRunner.h"

#include <memory>
#include <process.h>
#include <string>

#include "../Queue.h"

class ReadLoop {
public:
	ReadLoop(EventQueue &queue, std::string path);

	static unsigned int WINAPI startReadLoop(LPVOID arg);
	static void CALLBACK startRunner(__in ULONG_PTR arg);
	static void CALLBACK killReadLoop(__in ULONG_PTR arg);

  std::string getError();
  bool hasErrored();
  bool isWatching();

	~ReadLoop();
protected:
	void run();
	void shutdown();
	std::shared_ptr<ReadLoopRunner> *mRunner;
	std::wstring mDirectory;
	HANDLE mDirectoryHandle;
	EventQueue &mQueue;
private:
	unsigned int mThreadID;
	HANDLE mThread;
	bool mRunning;
};

#endif
