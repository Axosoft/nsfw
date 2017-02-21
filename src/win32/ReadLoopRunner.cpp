#include "../../includes/win32/ReadLoopRunner.h"

ReadLoopRunner::ReadLoopRunner(std::wstring directory, EventQueue &queue, HANDLE directoryHandle):
  mBufferSize(1024),
  mDirectory(directory),
  mDirectoryHandle(directoryHandle),
  mErrorMessage(""),
  mQueue(queue) {
  ZeroMemory(&mOverlapped, sizeof(OVERLAPPED));
  mBuffer = new BYTE[mBufferSize * BUFFER_KB];
  mSwap = new BYTE[mBufferSize * BUFFER_KB];
}

ReadLoopRunner::~ReadLoopRunner() {
  delete[] mBuffer;
  delete[] mSwap;
}

VOID CALLBACK ReadLoopRunner::eventCallback(DWORD errorCode, DWORD numBytes, LPOVERLAPPED overlapped) {
	std::shared_ptr<ReadLoopRunner> *runner = (std::shared_ptr<ReadLoopRunner> *)overlapped->hEvent;

  if (errorCode != ERROR_SUCCESS) {
    if (errorCode == ERROR_NOTIFY_ENUM_DIR) {
      runner->get()->setError("Buffer filled up and service needs a restart");
    } else if (errorCode == ERROR_INVALID_PARAMETER) {
      // resize the buffers because we're over the network, 64kb is the max buffer size for networked transmission
      runner->get()->resizeBuffers(64);
      if (!runner->get()->read()) {
        delete (std::shared_ptr<ReadLoopRunner> *)runner;
      }
      return;
    } else {
      runner->get()->setError("Service shutdown unexpectedly");
    }
  	delete (std::shared_ptr<ReadLoopRunner> *)runner;
    return;
  }

  runner->get()->swap(numBytes);
  BOOL readRequested = runner->get()->read();
  runner->get()->handleEvents();

  if (!readRequested) {
    delete (std::shared_ptr<ReadLoopRunner> *)runner;
  }
}

std::string ReadLoopRunner::getError() {
  return mErrorMessage;
}

std::wstring getWStringFileName(LPWSTR cFileName, DWORD length) {
  LPWSTR nullTerminatedFileName = new WCHAR[length + 1]();
  memcpy(nullTerminatedFileName, cFileName, length);
  std::wstring fileName = nullTerminatedFileName;
  delete[] nullTerminatedFileName;
  return fileName;
}

std::string ReadLoopRunner::getUTF8Directory(std::wstring path) {
  std::wstring::size_type found = path.rfind('\\');
  std::wstringstream utf16DirectoryStream;

  utf16DirectoryStream << mDirectory;

  if (found != std::wstring::npos) {
    utf16DirectoryStream
      << "\\"
      << path.substr(0, found);
  }

  std::wstring uft16DirectoryString = utf16DirectoryStream.str();
  int utf8length = WideCharToMultiByte(
    CP_UTF8,
    0,
    uft16DirectoryString.data(),
    -1,
    0,
    0,
    NULL,
    NULL
  );
  char *utf8CString = new char[utf8length];
  int failureToResolveToUTF8 = WideCharToMultiByte(
    CP_UTF8,
    0,
    uft16DirectoryString.data(),
    -1,
    utf8CString,
    utf8length,
    NULL,
    NULL
  );

  std::string utf8Directory = utf8CString;
  delete[] utf8CString;

  return utf8Directory;
}

std::string getUTF8FileName(std::wstring path) {
  std::wstring::size_type found = path.rfind('\\');
  if (found != std::wstring::npos) {
    path = path.substr(found + 1);
  }

  int utf8length = WideCharToMultiByte(
    CP_UTF8,
    0,
    path.data(),
    -1,
    0,
    0,
    NULL,
    NULL
    );

  // TODO: failure cases for widechar conversion
  char *utf8CString = new char[utf8length];
  int failureToResolveToUTF8 = WideCharToMultiByte(
    CP_UTF8,
    0,
    path.data(),
    -1,
    utf8CString,
    utf8length,
    NULL,
    NULL
    );

  std::string utf8Directory = utf8CString;
  delete[] utf8CString;

  return utf8Directory;
}

void ReadLoopRunner::handleEvents() {
  BYTE *base = mSwap;
  for (;;) {
    PFILE_NOTIFY_INFORMATION info = (PFILE_NOTIFY_INFORMATION)base;
    std::wstring fileName = getWStringFileName(info->FileName, info->FileNameLength);

    if (info->Action == FILE_ACTION_RENAMED_OLD_NAME) {
      if (info->NextEntryOffset != 0) {
        base += info->NextEntryOffset;
        info = (PFILE_NOTIFY_INFORMATION)base;
        if (info->Action == FILE_ACTION_RENAMED_NEW_NAME) {
          std::wstring fileNameNew = getWStringFileName(info->FileName, info->FileNameLength);

          mQueue.enqueue(
            RENAMED,
            getUTF8Directory(fileName),
            getUTF8FileName(fileName),
            getUTF8FileName(fileNameNew)
          );
        }
        else {
          mQueue.enqueue(DELETED, getUTF8Directory(fileName), getUTF8FileName(fileName));
          continue;
        }
      }
      else {
        mQueue.enqueue(DELETED, getUTF8Directory(fileName), getUTF8FileName(fileName));
        break;
      }
    }

    switch (info->Action) {
    case FILE_ACTION_ADDED:
    case FILE_ACTION_RENAMED_NEW_NAME: // in the case we just receive a new name and no old name in the buffer
      mQueue.enqueue(CREATED, getUTF8Directory(fileName), getUTF8FileName(fileName));
      break;
    case FILE_ACTION_REMOVED:
      mQueue.enqueue(DELETED, getUTF8Directory(fileName), getUTF8FileName(fileName));
      break;
    case FILE_ACTION_MODIFIED:
    default:
      mQueue.enqueue(MODIFIED, getUTF8Directory(fileName), getUTF8FileName(fileName));
    };

    if (info->NextEntryOffset == 0) {
      break;
    }
    base += info->NextEntryOffset;
  }
}

bool ReadLoopRunner::hasErrored() {
  return mErrorMessage != "";
}

BOOL ReadLoopRunner::read() {
  DWORD bytes;

  if (mDirectoryHandle == NULL) {
    return FALSE;
  }

  if (!ReadDirectoryChangesW(
    mDirectoryHandle,
    mBuffer,
    mBufferSize * BUFFER_KB,
    TRUE,
    FILE_NOTIFY_CHANGE_FILE_NAME
    | FILE_NOTIFY_CHANGE_DIR_NAME
    | FILE_NOTIFY_CHANGE_ATTRIBUTES
    | FILE_NOTIFY_CHANGE_SIZE
    | FILE_NOTIFY_CHANGE_LAST_WRITE
    | FILE_NOTIFY_CHANGE_LAST_ACCESS
    | FILE_NOTIFY_CHANGE_CREATION
    | FILE_NOTIFY_CHANGE_SECURITY,
    &bytes,
    &mOverlapped,
    &ReadLoopRunner::eventCallback
  )) {
    setError("Service shutdown unexpectedly");
    return FALSE;
  }

  return TRUE;
}

void ReadLoopRunner::resizeBuffers(unsigned int bufferSize) {
  mBufferSize = bufferSize;
  delete[] mBuffer;
  delete[] mSwap;
  mBuffer = new BYTE[mBufferSize * BUFFER_KB];
  mSwap = new BYTE[mBufferSize * BUFFER_KB];
}

void ReadLoopRunner::setError(std::string error) {
  mErrorMessage = error;
}

void ReadLoopRunner::setSharedPointer(std::shared_ptr<ReadLoopRunner> *ptr) {
  mOverlapped.hEvent = new std::shared_ptr<ReadLoopRunner>(*ptr);
}

void ReadLoopRunner::swap(DWORD numBytes) {
  memcpy(mSwap, mBuffer, numBytes);
}

void ReadLoopRunner::prepareForShutdown() {
  mDirectoryHandle = NULL;
}
