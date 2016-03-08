#include "../../includes/win32/FSWatcher.h"

#pragma managed
delegate void fsCallback(FileSystemEventArgs ^e);
delegate void errorCallback(ErrorEventArgs ^e);
delegate void renamedCallback(RenamedEventArgs ^e);

static void beginDispatchThread(Object ^fsWatcher) {
  ((FSWatcher ^)fsWatcher)->dispatchSetup();
}

FSWatcher::FSWatcher(Queue &queue, System::String ^path): mQueue(queue) {
  mDispatcherStarted = gcnew ManualResetEvent(false);

  Thread ^dispatchThread = gcnew Thread(gcnew ParameterizedThreadStart(&beginDispatchThread));
  dispatchThread->IsBackground = true;
  dispatchThread->Start(this);

  mDispatcherStarted->WaitOne();

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
  mDispatcher->BeginInvokeShutdown(DispatcherPriority::Normal);
}

void FSWatcher::dispatchSetup() {
  mDispatcher = Dispatcher::CurrentDispatcher;
  mDispatcherStarted->Set();
  mDispatcher->Run();
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

void FSEventHandler::eventHandlerHelper(EventType event, FileSystemEventArgs ^e) {
  System::String ^eventFileName = getFileName(e->Name);
  if (!System::String::IsNullOrEmpty(mFileName) && eventFileName != mFileName) {
    return;
  }

  char *directory = (char*)Marshal::StringToHGlobalAnsi(getDirectoryName(e->FullPath)).ToPointer();
  char *file = (char*)Marshal::StringToHGlobalAnsi(eventFileName).ToPointer();

  mQueue.enqueue(event, directory, file);

  Marshal::FreeHGlobal(IntPtr(directory));
  Marshal::FreeHGlobal(IntPtr(file));
}

void FSWatcher::onChangedDispatch(Object ^source, FileSystemEventArgs ^e) {
  array<Object ^> ^params = gcnew array<Object ^> { e };
  mDispatcher->BeginInvoke(gcnew fsCallback(this, &FSWatcher::onChanged), gcnew array<Object ^> { e });
}

void FSWatcher::onChanged(FileSystemEventArgs ^e) {
  eventHandlerHelper(MODIFIED, e);
}

void FSWatcher::onCreatedDispatch(Object ^source, FileSystemEventArgs ^e) {
	mDispatcher->BeginInvoke(gcnew fsCallback(this, &FSWatcher::onCreated), gcnew array<Object ^> { e });
}

void FSWatcher::onCreated(FileSystemEventArgs ^e) {
  eventHandlerHelper(CREATED, e);
}

void FSWatcher::onDeletedDispatch(Object ^source, FileSystemEventArgs ^e) {
	mDispatcher->BeginInvoke(gcnew fsCallback(this, &FSWatcher::onDeleted), gcnew array<Object ^> { e });
}

void FSWatcher::onDeleted(FileSystemEventArgs ^e) {
  eventHandlerHelper(DELETED, e);
}

void FSWatcher::onErrorDispatch(Object ^source, ErrorEventArgs ^e) {
	mDispatcher->BeginInvoke(gcnew errorCallback(this, &FSWatcher::onError), gcnew array<Object ^> { e });
}

void FSWatcher::onError(ErrorEventArgs ^e) {
  // TODO: error handling
  Console::WriteLine("Error");
}

void FSWatcher::onRenamedDispatch(Object ^source, RenamedEventArgs ^e) {
	mDispatcher->BeginInvoke(gcnew renamedCallback(this, &FSWatcher::onRenamed), gcnew array<Object ^> { e });
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
