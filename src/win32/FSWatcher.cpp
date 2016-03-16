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
      Sleep(50);
      continue;
    }

    for (Int32 i = 0; i < count; ++i) {
      if (!mDispatching) {
        return;
      }
      Tuple<Int32, Object^> ^toDispatch;
      while (!mDispatchQueue->TryDequeue(toDispatch)) {
        if (!mDispatching) {
          return;
        }
        Sleep(1);
      }

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
  System::Text::Encoding ^utf8 = System::Text::Encoding::UTF8;
  System::Text::Encoding ^utf16 = System::Text::Encoding::Unicode;

  array<Byte> ^utf16Directory = utf16->GetBytes(getDirectoryName(e->FullPath));
  array<Byte> ^utf16File = utf16->GetBytes(getFileName(e->Name));

  array<Byte> ^directoryBytesTemp = System::Text::Encoding::Convert(utf16, utf8, utf16Directory);
  array<Byte> ^fileBytesTemp = System::Text::Encoding::Convert(utf16, utf8, utf16File);

  array<Byte> ^directoryBytes = gcnew array<Byte>(directoryBytesTemp->Length + 1);
  array<Byte> ^fileBytes = gcnew array<Byte>(fileBytesTemp->Length + 1);

  for (int i = 0; i < directoryBytesTemp->Length; ++i) {
    directoryBytes[i] = directoryBytesTemp[i];
  }

  for (int i = 0; i < fileBytesTemp->Length; ++i) {
    fileBytes[i] = fileBytesTemp[i];
  }

  directoryBytes[directoryBytesTemp->Length] = 0;
  fileBytes[fileBytesTemp->Length] = 0;

  IntPtr directoryBytesPointer = Marshal::AllocHGlobal(directoryBytes->Length);
  IntPtr fileBytesPointer = Marshal::AllocHGlobal(fileBytes->Length);

  Marshal::Copy(directoryBytes, 0, directoryBytesPointer, directoryBytes->Length);
  Marshal::Copy(fileBytes, 0, fileBytesPointer, fileBytes->Length);

  mQueue.enqueue(
    event,
    (char *)directoryBytesPointer.ToPointer(),
    (char *)fileBytesPointer.ToPointer()
  );

  Marshal::FreeHGlobal(directoryBytesPointer);
  Marshal::FreeHGlobal(fileBytesPointer);
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
  System::Text::Encoding ^utf8 = System::Text::Encoding::UTF8;
  System::Text::Encoding ^utf16 = System::Text::Encoding::Unicode;

  array<Byte> ^utf16Directory = utf16->GetBytes(getDirectoryName(e->FullPath));
  array<Byte> ^utf16OldFile = utf16->GetBytes(getFileName(e->OldName));
  array<Byte> ^utf16NewFile = utf16->GetBytes(getFileName(e->Name));

  array<Byte> ^directoryBytesTemp = System::Text::Encoding::Convert(utf16, utf8, utf16Directory);
  array<Byte> ^oldFileBytesTemp = System::Text::Encoding::Convert(utf16, utf8, utf16OldFile);
  array<Byte> ^newFileBytesTemp = System::Text::Encoding::Convert(utf16, utf8, utf16NewFile);

  array<Byte> ^directoryBytes = gcnew array<Byte>(directoryBytesTemp->Length + 1);
  array<Byte> ^oldFileBytes = gcnew array<Byte>(oldFileBytesTemp->Length + 1);
  array<Byte> ^newFileBytes = gcnew array<Byte>(newFileBytesTemp->Length + 1);

  for (int i = 0; i < directoryBytesTemp->Length; ++i) {
    directoryBytes[i] = directoryBytesTemp[i];
  }

  for (int i = 0; i < oldFileBytesTemp->Length; ++i) {
    oldFileBytes[i] = oldFileBytesTemp[i];
  }

  for (int i = 0; i < newFileBytesTemp->Length; ++i) {
    newFileBytes[i] = newFileBytesTemp[i];
  }

  directoryBytes[directoryBytesTemp->Length] = 0;
  oldFileBytes[oldFileBytesTemp->Length] = 0;
  newFileBytes[newFileBytesTemp->Length] = 0;


  IntPtr directoryBytesPointer = Marshal::AllocHGlobal(directoryBytes->Length);
  IntPtr oldFileBytesPointer = Marshal::AllocHGlobal(oldFileBytes->Length);
  IntPtr newFileBytesPointer = Marshal::AllocHGlobal(newFileBytes->Length);

  Marshal::Copy(directoryBytes, 0, directoryBytesPointer, directoryBytes->Length);
  Marshal::Copy(oldFileBytes, 0, oldFileBytesPointer, oldFileBytes->Length);
  Marshal::Copy(newFileBytes, 0, newFileBytesPointer, newFileBytes->Length);

  mQueue.enqueue(
    RENAMED,
    (char *)directoryBytesPointer.ToPointer(),
    (char *)oldFileBytesPointer.ToPointer(),
    (char *)newFileBytesPointer.ToPointer()
  );

  Marshal::FreeHGlobal(directoryBytesPointer);
  Marshal::FreeHGlobal(oldFileBytesPointer);
  Marshal::FreeHGlobal(newFileBytesPointer);
}
