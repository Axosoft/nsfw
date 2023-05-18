#include "../../includes/linux/FanotifyEventLoop.h"

FanotifyEventLoop::FanotifyEventLoop(
  int fanotifyInstance,
  FanotifyService *fanotifyService
) :
  mFanotifyInstance(fanotifyInstance),
  mFanotifyService(fanotifyService)
{
  mStarted = !pthread_create(
    &mEventLoop,
    NULL,
    [](void *eventLoop)->void * {
      ((FanotifyEventLoop *)eventLoop)->work();
      return NULL;
    },
    (void *)this
  );
    if (mStarted) { mLoopingSemaphore.wait(); }
}

bool FanotifyEventLoop::isLooping() {
  return mStarted;
}

void FanotifyEventLoop::work() {
  char buffer[BUFFER_SIZE];
  char *pathname;
  fanotify_event_metadata *event = NULL;
  unsigned int bytesRead;
  FanotifyService *fanotifyService = mFanotifyService;
  FanotifyRenameEvent renameEvent;
  renameEvent.isStarted = false;

  mLoopingSemaphore.signal();
  while((bytesRead = read(mFanotifyInstance, &buffer, BUFFER_SIZE)) > 0) {
    std::lock_guard<std::mutex> syncWithDestructor(mMutex);
    for (event = (struct fanotify_event_metadata *) buffer;
        FAN_EVENT_OK(event, bytesRead);
        event = FAN_EVENT_NEXT(event, bytesRead)) {
      struct fanotify_event_info_fid *fid = (struct fanotify_event_info_fid *) (event + 1);
      struct file_handle *file_handle = (struct file_handle *)fid->handle;

      if (fid->hdr.info_type == FAN_EVENT_INFO_TYPE_DFID_NAME) {
        pathname = (char *)file_handle->f_handle + file_handle->handle_bytes;
      } else {
        continue;
      }

      int event_fd = open_by_handle_at(AT_FDCWD, file_handle, O_RDONLY);
      if (event_fd == -1) {
        continue;
      }

      if (event->mask & (uint32_t)(FAN_ATTRIB | FAN_MODIFY)) {
        fanotifyService->modify(event_fd, pathname);
      } else if (event->mask & (uint32_t)FAN_CREATE) {
        fanotifyService->create(event_fd, pathname);
      } else if (event->mask & (uint32_t)(FAN_DELETE | FAN_DELETE_SELF)) {
        fanotifyService->remove(event_fd, pathname);
      } else if (event->mask & (uint32_t)FAN_MOVED_TO) {
        if (!renameEvent.isStarted) {
          fanotifyService->create(event_fd, pathname);
          close(event_fd);
          continue;
        }

        fanotifyService->rename(renameEvent.fd, renameEvent.name, event_fd, pathname);
        close(renameEvent.fd);
        renameEvent.isStarted = false;
      } else if (event->mask & (uint32_t)FAN_MOVED_FROM) {
        renameEvent.name = pathname;
        renameEvent.fd = event_fd;
        renameEvent.isStarted = true;
        continue;
      }

      close(event_fd);
    }

    if (renameEvent.isStarted) {
      fanotifyService->remove(renameEvent.fd, renameEvent.name);
      renameEvent.isStarted = false;
    }
  }
  mStarted = false;
}

FanotifyEventLoop::~FanotifyEventLoop() {
  if (!mStarted) {
    return;
  }

  {
    std::lock_guard<std::mutex> syncWithWork(mMutex);
    pthread_cancel(mEventLoop);
  }

  pthread_join(mEventLoop, NULL);
}
