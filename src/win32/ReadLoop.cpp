#include "../../includes/win32/ReadLoop.h"

ReadLoop::ReadLoop(EventQueue &queue, std::string path):
  mDirectoryHandle(NULL),
  mQueue(queue),
  mRunner(NULL),
  mThread(NULL),
	mThreadID(0) {

	int wlen = MultiByteToWideChar(CP_UTF8, 0, path.data(), -1, 0, 0);

	if (wlen == 0) {
		return;
	}

	LPWSTR outputBuffer = new WCHAR[wlen]();

	int failureToResolveUTF8 = MultiByteToWideChar(CP_UTF8, 0, path.data(), -1, outputBuffer, wlen);

	if (failureToResolveUTF8 == 0) {
		delete[] outputBuffer;
		return;
	}

	mDirectoryHandle = CreateFileW(
		outputBuffer,
		FILE_LIST_DIRECTORY,
		FILE_SHARE_READ
		| FILE_SHARE_WRITE
		| FILE_SHARE_DELETE,
		NULL,
		OPEN_EXISTING,
		FILE_FLAG_BACKUP_SEMANTICS
		| FILE_FLAG_OVERLAPPED,
		NULL
	);

	mDirectory = outputBuffer;

	delete[] outputBuffer;
	outputBuffer = NULL;

	if (mDirectoryHandle == INVALID_HANDLE_VALUE) {
    mDirectoryHandle = NULL;
		return;
	}

	// TODO: handle errors
	mThread = (HANDLE)_beginthreadex(
		NULL,
		0,
		ReadLoop::startReadLoop,
		this,
		0,
		&mThreadID
	);

  if (!mThread) {
    CloseHandle(mDirectoryHandle);
    mDirectoryHandle = NULL;
    mThread = NULL;
    return;
  }

	QueueUserAPC(ReadLoop::startRunner, mThread, (ULONG_PTR)this);
  int maxWait = 10 * 1000; // after 10 seconds, abandon hope
  while (mRunner == NULL && maxWait > 0) {
    Sleep(50);
    maxWait--;
  }
}

std::string ReadLoop::getError() {
  if (mDirectoryHandle == NULL || mThread == NULL) {
    return "Failed to start watcher";
  } else if (mRunner == NULL) {
    return "Watcher is not started";
  } else {
    return mRunner->get()->getError();
  }
}

bool ReadLoop::hasErrored() {
  return mDirectoryHandle == NULL || mThread == NULL || (mRunner != NULL && mRunner->get()->hasErrored());
}

bool ReadLoop::isWatching() {
  return mDirectoryHandle != NULL && mThread != NULL && mRunner != NULL && !mRunner->get()->hasErrored();
}

unsigned int WINAPI ReadLoop::startReadLoop(LPVOID arg) {
	ReadLoop *readLoop = (ReadLoop *)arg;
	readLoop->run();
	return 0;
}

void ReadLoop::run() {
	while(mDirectoryHandle != NULL) {
		SleepEx(INFINITE, true);
	}
}

void CALLBACK ReadLoop::startRunner(__in ULONG_PTR arg) {
	ReadLoop *readLoop = (ReadLoop *)arg;
	readLoop->mRunner = new std::shared_ptr<ReadLoopRunner>(new ReadLoopRunner(
		readLoop->mDirectory,
		readLoop->mQueue,
		readLoop->mDirectoryHandle
	));
	readLoop->mRunner->get()->setSharedPointer(readLoop->mRunner);
	readLoop->mRunner->get()->read();
}

void CALLBACK ReadLoop::killReadLoop(__in ULONG_PTR arg) {
	ReadLoop *readLoop = (ReadLoop *)arg;
	readLoop->shutdown();
}

void ReadLoop::shutdown() {
	if (mRunner != NULL) {
		mRunner->get()->prepareForShutdown();
		delete mRunner;
		mRunner = NULL;
	}

	CancelIo(mDirectoryHandle);
	CloseHandle(mDirectoryHandle);
	mDirectoryHandle = NULL;
}

ReadLoop::~ReadLoop() {
	if (mThread) {
		QueueUserAPC(ReadLoop::killReadLoop, mThread, (ULONG_PTR)this);
		WaitForSingleObjectEx(mThread, 10000, true);
		CloseHandle(mThread);

		mThread = NULL;
		mThreadID = 0;
	}
}
