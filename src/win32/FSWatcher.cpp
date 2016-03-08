#include "../../includes/win32/FSWatcher.h"

#pragma managed
delegate void fsCallback(FileSystemEventArgs ^e);
delegate void errorCallback(ErrorEventArgs ^e);
delegate void renamedCallback(RenamedEventArgs ^e);


static void beginDispatchThread(Object ^fsWatcher) {
  ((FSWatcher ^)fsWatcher)->dispatchSetup();
}

FSWatcher::FSWatcher(System::String ^path) {
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

void FSWatcher::onChangedDispatch(Object ^source, FileSystemEventArgs ^e) {
  array<Object ^> ^params = gcnew array<Object ^> { e };
  mDispatcher->BeginInvoke(gcnew fsCallback(this, &FSWatcher::onChanged), gcnew array<Object ^> { e });
}

void FSWatcher::onChanged(FileSystemEventArgs ^e) {
  Console::WriteLine("Changed {0}", e->FullPath);
}

void FSWatcher::onCreatedDispatch(Object ^source, FileSystemEventArgs ^e) {
	mDispatcher->BeginInvoke(gcnew fsCallback(this, &FSWatcher::onCreated), gcnew array<Object ^> { e });
}

void FSWatcher::onCreated(FileSystemEventArgs ^e) {
  Console::WriteLine("Created {0}", e->FullPath);
}

void FSWatcher::onDeletedDispatch(Object ^source, FileSystemEventArgs ^e) {
	mDispatcher->BeginInvoke(gcnew fsCallback(this, &FSWatcher::onDeleted), gcnew array<Object ^> { e });
}

void FSWatcher::onDeleted(FileSystemEventArgs ^e) {
  Console::WriteLine("Deleted {0}", e->FullPath);
}

void FSWatcher::onErrorDispatch(Object ^source, ErrorEventArgs ^e) {
	mDispatcher->BeginInvoke(gcnew errorCallback(this, &FSWatcher::onError), gcnew array<Object ^> { e });
}

void FSWatcher::onError(ErrorEventArgs ^e) {
  Console::WriteLine("Error");
}

void FSWatcher::onRenamedDispatch(Object ^source, RenamedEventArgs ^e) {
	mDispatcher->BeginInvoke(gcnew renamedCallback(this, &FSWatcher::onRenamed), gcnew array<Object ^> { e });
}

void FSWatcher::onRenamed(RenamedEventArgs ^e) {
  Console::WriteLine("Renamed {0} to {1}", e->OldFullPath, e->FullPath);
}
