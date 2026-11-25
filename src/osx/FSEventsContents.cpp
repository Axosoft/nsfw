#include "../../includes/osx/FSEventsService.h"
#include <climits>
#include <dirent.h>
#include <fcntl.h>
#include <unistd.h>

// FSEvents reports a moved-in directory but can omit its children.
void FSEventsService::createContents(const std::string &directory, uint64_t inode, dev_t device) {
  if (!mRecursive) {
    return;
  }
  struct Directory {
    std::string path;
    uint64_t inode;
    dev_t device;
  };
  std::vector<Directory> pending{{directory, inode, device}};
  while (!pending.empty() && !mStopping) {
    const Directory current = std::move(pending.back());
    pending.pop_back();
    std::string parent, name;
    splitFilePath(parent, name, current.path);
    if (isExcludedDirectory(current.path) || (mFilter && mFilter->isDirectoryIgnored(parent, name))) {
      continue;
    }
    // A directory queued for scanning or any of its ancestors may have been replaced meanwhile.
    const int descriptor = open(current.path.c_str(), O_RDONLY | O_DIRECTORY | O_NOFOLLOW_ANY | O_CLOEXEC);
    if (descriptor < 0) {
      continue;
    }
    struct stat opened;
    if (fstat(descriptor, &opened) != 0 || (uint64_t)opened.st_ino != current.inode || opened.st_dev != current.device) {
      close(descriptor);
      continue;
    }
    DIR *handle = fdopendir(descriptor);
    if (handle == NULL) {
      close(descriptor);
      continue;
    }
    while (!mStopping) {
      struct dirent *entry = readdir(handle);
      if (entry == NULL) {
        break;
      }
      const std::string name = entry->d_name;
      if (name == "." || name == "..") {
        continue;
      }
      const std::string child = current.path + "/" + name;
      struct stat file;
      if (fstatat(descriptor, name.c_str(), &file, AT_SYMLINK_NOFOLLOW) != 0) {
        continue;
      }
      // A descriptor can stay open after its directory has moved outside the watch.
      char openedPath[PATH_MAX];
      if (fcntl(descriptor, F_GETPATH, openedPath) != 0 || current.path != openedPath ||
        isExcludedDirectory(current.path)) {
        break;
      }
      uint64_t reported;
      if (!lookUpReported(child, reported) || reported != (uint64_t)file.st_ino) {
        create(child, file.st_ino, kindFromMode(file.st_mode));
      }
      if (S_ISDIR(file.st_mode)) {
        pending.push_back({child, (uint64_t)file.st_ino, file.st_dev});
      }
    }
    closedir(handle);
  }
}
