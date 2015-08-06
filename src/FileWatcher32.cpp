#include "includes/FileWatcher32.h"

namespace NSFW {
  FSEventHandler::FSEventHandler(std::queue<Event> &eventsQueue)
   : mEventsQueue(eventsQueue) {}

  FSEventHandler::OnChanged(Object^ source, FileSystemEventArgs^ e) {
    Event event;
    event.directory = e->FullPath;
    event.action = e->ChangeType;
  }

  FSEventHandler::OnRenamed(Object^ source, RenamedEventArgs^ e) {
    Event event;
    event.directory = e->FullPath;
    event.oldDirectory = e->OldFullPath;
    event.action = e->ChangeType;
  }

  FileWatcher32::FileWatcher32(std::string path, std::queue<Event> &eventsQueue)
  : mEventsQueue(eventsQueue) {
    fsWatcher = gcnew FileSystemWatcher( );
    fsWatcher->Path = path;
    fsWatcher->NotifyFilter = static_cast<NotifyFilters>
               (NotifyFilters::FileName |
                NotifyFilters::Attributes |
                NotifyFilters::LastAccess |
                NotifyFilters::LastWrite |
                NotifyFilters::Security |
                NotifyFilters::Size );

    handler = gcnew FSEventHandler(eventsQueue);
     fsWatcher->Changed += gcnew FileSystemEventHandler(
             handler, &FSEventHandler::OnChanged);
     fsWatcher->Created += gcnew FileSystemEventHandler(
             handler, &FSEventHandler::OnChanged);
     fsWatcher->Deleted += gcnew FileSystemEventHandler(
             handler, &FSEventHandler::OnChanged);
     fsWatcher->Renamed += gcnew RenamedEventHandler(
             handler, &FSEventHandler::OnRenamed);

     fsWatcher->EnableRaisingEvents = true;
  }

}
