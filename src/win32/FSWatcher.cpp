#include "../../includes/win32/FSWatcher.h"

#pragma managed
static void beginDispatchThread(Object ^fsWatcher) {
  ((FSWatcher ^)fsWatcher)->dispatchSetup();
}

[PermissionSet(SecurityAction::Demand, Name="FullTrust")]
FSWatcher::FSWatcher(EventQueue &queue, System::String ^path):
  mDispatching(false),
  mQueue(queue) {
  mDispatchQueue = gcnew ConcurrentQueue< Tuple<Int32, Object ^> ^>();

  mDispatchThread = gcnew Thread(gcnew ParameterizedThreadStart(&beginDispatchThread));
  mDispatchThread->IsBackground = true;
  mDispatchThread->Start(this);

  mWatcher = gcnew FileSystemWatcher();
  mWatcher->Path = path;
  mWatcher->InternalBufferSize = 64 * 1024;
  mWatcher->IncludeSubdirectories = true;
  mWatcher->NotifyFilter = static_cast<NotifyFilters>(
    NotifyFilters::FileName |
    NotifyFilters::DirectoryName |
    NotifyFilters::Attributes |
    NotifyFilters::LastWrite |
    NotifyFilters::Security |
    NotifyFilters::Size
  );

  mWatcher->Changed += gcnew FileSystemEventHandler(this, &FSWatcher::onChangedDispatch);
  mWatcher->Created += gcnew FileSystemEventHandler(this, &FSWatcher::onCreatedDispatch);
  mWatcher->Deleted += gcnew FileSystemEventHandler(this, &FSWatcher::onDeletedDispatch);
  mWatcher->Error += gcnew ErrorEventHandler(this, &FSWatcher::onErrorDispatch);
  mWatcher->Renamed += gcnew RenamedEventHandler(this, &FSWatcher::onRenamedDispatch);

  mWatcher->EnableRaisingEvents = true;
}

FSWatcher::~FSWatcher() {
  delete mWatcher;
  mDispatching = false;
  mDispatchThread->Join();
}

void FSWatcher::dispatchSetup() {
  mDispatching = true;
  while (mDispatching) {
    Int32 count = mDispatchQueue->Count;
    if (count == 0) {
      continue;
    }

    for (Int32 i = 0; i < count; ++i) {
      if (!mDispatching) {
        break;
      }
      Tuple<Int32, Object^> ^toDispatch;
      while (!mDispatchQueue->TryDequeue(toDispatch)) {}
      if (toDispatch->Item1 == RENAMED) {
        onRenamed(safe_cast<RenamedEventArgs ^>(toDispatch->Item2));
      } else {
        eventHandlerHelper(static_cast<EventType>(toDispatch->Item1), safe_cast<FileSystemEventArgs ^>(toDispatch->Item2));
      }
    }
  }
}

System::String^ getDirectoryName(System::String^ path)
{
  wchar_t delim = '\\';
  array<System::String^>^ tokens = path->Split(delim);

  if (path[path->Length - 1] == delim)
  {
    if (
      tokens->Length == 2 &&
      System::String::IsNullOrEmpty(tokens[1]) ||
      tokens->Length < 2
    ) {
      return gcnew System::String("");
    }
    else
    {
      return path->Substring(0, path->Length - 1);
    }
  }
  else
  {
    return path->Substring(0, path->LastIndexOf(delim));
  }
}

System::String^ getFileName(System::String^ path)
{
  wchar_t delim = '\\';
  if (path->LastIndexOf(delim) == path->Length - 1)
  {
    return gcnew System::String("");
  }
  else
  {
    return path->Substring(path->LastIndexOf(delim) + 1);
  }
}

void FSWatcher::eventHandlerHelper(EventType event, FileSystemEventArgs ^e) {
  System::String ^eventFileName = getFileName(e->Name);

  char *directory = (char*)Marshal::StringToHGlobalAnsi(getDirectoryName(e->FullPath)).ToPointer();
  char *file = (char*)Marshal::StringToHGlobalAnsi(eventFileName).ToPointer();

  mQueue.enqueue(event, directory, file);

  Marshal::FreeHGlobal(IntPtr(directory));
  Marshal::FreeHGlobal(IntPtr(file));
}

System::String ^FSWatcher::getError() {
  return mError;
}

bool FSWatcher::hasErrored() {
  return !System::String::IsNullOrEmpty(mError);
}

void FSWatcher::onChangedDispatch(Object ^source, FileSystemEventArgs ^e) {
  mDispatchQueue->Enqueue(gcnew Tuple<Int32, Object ^>(MODIFIED, e));
}

void FSWatcher::onCreatedDispatch(Object ^source, FileSystemEventArgs ^e) {
  mDispatchQueue->Enqueue(gcnew Tuple<Int32, Object ^>(CREATED, e));
}

void FSWatcher::onDeletedDispatch(Object ^source, FileSystemEventArgs ^e) {
  mDispatchQueue->Enqueue(gcnew Tuple<Int32, Object ^>(DELETED, e));
}

void FSWatcher::onErrorDispatch(Object ^source, ErrorEventArgs ^e) {
  mDispatching = false;
  mWatcher->EnableRaisingEvents = false;
  mError = e->GetException()->Message;
}

void FSWatcher::onRenamedDispatch(Object ^source, RenamedEventArgs ^e) {
  mDispatchQueue->Enqueue(gcnew Tuple<Int32, Object ^>(RENAMED, e));
}

void FSWatcher::onRenamed(RenamedEventArgs ^e) {
  System::String ^eventFileName = getFileName(e->OldName);
  char *directory = (char*)Marshal::StringToHGlobalAnsi(getDirectoryName(e->FullPath)).ToPointer();
  char *fileA = (char*)Marshal::StringToHGlobalAnsi(eventFileName).ToPointer();
  char *fileB = (char*)Marshal::StringToHGlobalAnsi(getFileName(e->Name)).ToPointer();

  mQueue.enqueue(RENAMED, directory, fileA, fileB);

  Marshal::FreeHGlobal(IntPtr(directory));
  Marshal::FreeHGlobal(IntPtr(fileA));
  Marshal::FreeHGlobal(IntPtr(fileB));
}
