#include "../includes/FilterPaths.h"

#include <cctype>
#include <cerrno>
#include <cstdio>
#include <cstdlib>
#include <vector>
#include <sys/stat.h>

#if defined(_WIN32)
#include <Windows.h>
#include <fcntl.h>
#include <io.h>
#else
#include <fcntl.h>
#include <unistd.h>
#endif

namespace filterpaths {

namespace {

char lowerAscii(char c) {
  return c >= 'A' && c <= 'Z' ? static_cast<char>(c - 'A' + 'a') : c;
}

}


bool startsWith(const std::string &value, const std::string &prefix, bool caseSensitive) {
  if (value.size() < prefix.size()) {
    return false;
  }
  for (size_t i = 0; i < prefix.size(); ++i) {
    if (caseSensitive ? value[i] != prefix[i] : lowerAscii(value[i]) != lowerAscii(prefix[i])) {
      return false;
    }
  }
  return true;
}

// '/' separated, the way every path is kept here.
std::string toGeneric(std::string path) {
#if defined(_WIN32)
  for (char &c : path) {
    if (c == '\\') c = '/';
  }
#endif
  return path;
}

std::string fromGeneric(std::string path) {
#if defined(_WIN32)
  for (char &c : path) {
    if (c == '/') c = '\\';
  }
#endif
  return path;
}

#if defined(_WIN32)
std::wstring toWide(const std::string &value) {
  const int length = MultiByteToWideChar(CP_UTF8, 0, value.data(), -1, NULL, 0);
  if (length <= 0) {
    return std::wstring();
  }
  std::wstring wide(length - 1, L'\0');
  if (MultiByteToWideChar(CP_UTF8, 0, value.data(), -1, &wide[0], length) == 0) {
    return std::wstring();
  }
  return wide;
}

std::string fromWide(const std::wstring &value) {
  const int length = WideCharToMultiByte(CP_UTF8, 0, value.data(), -1, NULL, 0, NULL, NULL);
  if (length <= 0) {
    return std::string();
  }
  std::string narrow(length - 1, '\0');
  if (WideCharToMultiByte(CP_UTF8, 0, value.data(), -1, &narrow[0], length, NULL, NULL) == 0) {
    return std::string();
  }
  return narrow;
}
#endif

// What is at a native path, following symlinks.
EntryKind lookUp(const std::string &path) {
#if defined(_WIN32)
  const DWORD attributes = GetFileAttributesW(toWide(path).c_str());
  if (attributes == INVALID_FILE_ATTRIBUTES) {
    return EntryKind::UNKNOWN;
  }
  return (attributes & FILE_ATTRIBUTE_DIRECTORY) ? EntryKind::DIRECTORY : EntryKind::FILE;
#else
  struct stat file;
  if (stat(path.c_str(), &file) != 0) {
    return EntryKind::UNKNOWN;
  }
  return S_ISDIR(file.st_mode) ? EntryKind::DIRECTORY : EntryKind::FILE;
#endif
}

// As git: a larger ignore file is skipped. Every other file read here is far smaller.
const long long MAX_FILE_SIZE = 100LL * 1024 * 1024;

#if !defined(_WIN32)
// A file git would read with no rules: absent, or one it isn't allowed to read.
bool isMissing(int error) {
  return error == ENOENT || error == ENOTDIR || error == EACCES || error == EPERM;
}
#endif

// The contents of the regular file at a native path, taken from the repository being watched, so neither a FIFO nor a
// device nor a huge file can block or exhaust the watcher. MISSING when there is none, FAILED when it may be there
// but can't be read now (a sharing violation on Windows, no descriptor left), which isn't the same as no rules. An
// in-tree .gitignore that is a symlink counts as missing, as in git.
ReadResult readFile(const std::string &path, std::string &contents, bool ignoreFile) {
  contents.clear();
#if defined(_WIN32)
  const std::wstring widePath = toWide(path);
  const DWORD attributes = GetFileAttributesW(widePath.c_str());
  if (attributes == INVALID_FILE_ATTRIBUTES) {
    // Denied for good is no rules too, as git takes it.
    const DWORD error = GetLastError();
    return error == ERROR_FILE_NOT_FOUND || error == ERROR_PATH_NOT_FOUND || error == ERROR_ACCESS_DENIED ?
      ReadResult::MISSING : ReadResult::FAILED;
  }
  if (attributes & FILE_ATTRIBUTE_DIRECTORY) {
    return ReadResult::MISSING;
  }
  // Shared for deletion too, so a checkout or an editor replacing the file while it is read isn't refused.
  // An in-tree .gitignore is opened as itself, not where a link leads, and checked on the handle, so it can't become a
  // link in between.
  HANDLE handle = CreateFileW(widePath.c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
    NULL, OPEN_EXISTING, ignoreFile ? FILE_FLAG_OPEN_REPARSE_POINT : FILE_ATTRIBUTE_NORMAL, NULL);
  if (handle == INVALID_HANDLE_VALUE) {
    // Denied here, though its attributes could be read, is most likely a file being deleted: read again later.
    const DWORD error = GetLastError();
    return error == ERROR_FILE_NOT_FOUND || error == ERROR_PATH_NOT_FOUND ? ReadResult::MISSING : ReadResult::FAILED;
  }
  if (ignoreFile) {
    FILE_ATTRIBUTE_TAG_INFO tag;
    if (!GetFileInformationByHandleEx(handle, FileAttributeTagInfo, &tag, sizeof(tag))) {
      // A filesystem that can't tell: a reparse point is skipped, since it can't be told from a link.
      tag.FileAttributes = attributes;
      tag.ReparseTag = IO_REPARSE_TAG_SYMLINK;
    }
    if (tag.FileAttributes & FILE_ATTRIBUTE_REPARSE_POINT) {
      // Only a link, as git tells it.
      if (tag.ReparseTag == IO_REPARSE_TAG_SYMLINK || tag.ReparseTag == IO_REPARSE_TAG_MOUNT_POINT) {
        CloseHandle(handle);
        return ReadResult::MISSING;
      }
      // A cloud placeholder or a deduplicated file is a reparse point too, whose data only comes through a normal
      // open: the same file, checked by its identity.
      BY_HANDLE_FILE_INFORMATION before, after;
      const HANDLE data = CreateFileW(widePath.c_str(), GENERIC_READ,
        FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, NULL, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL);
      const bool same = data != INVALID_HANDLE_VALUE && GetFileInformationByHandle(handle, &before) &&
        GetFileInformationByHandle(data, &after) && before.dwVolumeSerialNumber == after.dwVolumeSerialNumber &&
        before.nFileIndexHigh == after.nFileIndexHigh && before.nFileIndexLow == after.nFileIndexLow;
      const DWORD error = data == INVALID_HANDLE_VALUE ? GetLastError() : ERROR_SUCCESS;
      CloseHandle(handle);
      if (!same) {
        if (data != INVALID_HANDLE_VALUE) CloseHandle(data);
        // One no normal open can read (a WSL symlink, a socket) has no rules, as for git; another file in between
        // is read again later.
        return error == ERROR_CANT_ACCESS_FILE || error == ERROR_FILE_NOT_FOUND || error == ERROR_PATH_NOT_FOUND ?
          ReadResult::MISSING : ReadResult::FAILED;
      }
      handle = data;
    }
  }
  // A device (COM1, NUL) named by a config could block a read.
  if (GetFileType(handle) != FILE_TYPE_DISK) {
    CloseHandle(handle);
    return ReadResult::MISSING;
  }
  const int descriptor = _open_osfhandle(reinterpret_cast<intptr_t>(handle), _O_RDONLY);
  if (descriptor == -1) {
    CloseHandle(handle);
    return ReadResult::FAILED;
  }
  FILE *file = _fdopen(descriptor, "rb");
  if (file == NULL) {
    _close(descriptor);
    return ReadResult::FAILED;
  }
#else
  // Non-blocking, so opening a FIFO doesn't wait for a writer.
  const int descriptor = open(path.c_str(), O_RDONLY | O_NONBLOCK | O_CLOEXEC | (ignoreFile ? O_NOFOLLOW : 0));
  if (descriptor == -1) {
    // O_NOFOLLOW on a symlink: ELOOP, or EMLINK and EFTYPE on FreeBSD.
    bool refusedLink = errno == ELOOP;
#ifdef EMLINK
    refusedLink = refusedLink || errno == EMLINK;
#endif
#ifdef EFTYPE
    refusedLink = refusedLink || errno == EFTYPE;
#endif
    return isMissing(errno) || (ignoreFile && refusedLink) ? ReadResult::MISSING : ReadResult::FAILED;
  }
  struct stat status;
  if (fstat(descriptor, &status) != 0 || !S_ISREG(status.st_mode) || status.st_size > MAX_FILE_SIZE) {
    close(descriptor);
    return ReadResult::MISSING;
  }
  FILE *file = fdopen(descriptor, "rb");
  if (file == NULL) {
    close(descriptor);
    return ReadResult::FAILED;
  }
#endif
  char buffer[4096];
  size_t read;
  bool tooLarge = false;
  while ((read = fread(buffer, 1, sizeof(buffer), file)) > 0) {
    contents.append(buffer, read);
    if ((long long)contents.size() > MAX_FILE_SIZE) {
      tooLarge = true;
      break;
    }
  }
  const bool failed = ferror(file) != 0;
  fclose(file);
  if (tooLarge) {
    contents.clear();
    return ReadResult::MISSING;
  }
  return failed ? ReadResult::FAILED : ReadResult::READ;
}


// How long the root of a '/' separated absolute path is, its separator included: "/", "C:/", "//?/C:/",
// "//server/share/" or "//?/UNC/server/share/".
size_t rootLength(const std::string &path) {
#if defined(_WIN32)
  size_t start = 0;
  if (startsWith(path, "//?/UNC/", false)) {
    start = 8;
  } else if (startsWith(path, "//?/", true)) {
    return path.size() >= 7 ? 7 : path.size();
  } else if (startsWith(path, "//", true)) {
    start = 2;
  } else {
    return path.size() >= 3 && path[1] == ':' ? 3 : 0;
  }
  const size_t server = path.find('/', start);
  if (server == std::string::npos) return path.size();
  const size_t share = path.find('/', server + 1);
  return share == std::string::npos ? path.size() : share + 1;
#else
  return !path.empty() && path[0] == '/' ? 1 : 0;
#endif
}

std::string join(const std::string &directory, const std::string &name) {
  return directory.empty() || directory.back() == '/' ? directory + name : directory + "/" + name;
}

bool isAbsolute(const std::string &path) {
  return rootLength(path) > 0;
}

std::string withoutTrailingSeparators(std::string path) {
  const size_t root = rootLength(path);
  while (path.size() > root && path.back() == '/') {
    path.pop_back();
  }
  return path;
}

// The parent of a '/' separated absolute path, empty for a root.
std::string parentOf(const std::string &path) {
  const size_t root = rootLength(path);
  if (path.size() <= root) {
    return std::string();
  }
  const size_t slash = path.rfind('/');
  return slash == std::string::npos || slash < root ? path.substr(0, root) : path.substr(0, slash);
}

// relative resolved against a '/' separated absolute directory, with its "." and ".." names resolved as text.
std::string resolve(const std::string &directory, const std::string &relative) {
  const std::string path = isAbsolute(relative) ? relative : join(directory, relative);
  const size_t root = rootLength(path);
  std::vector<std::string> names;
  for (size_t start = root; start <= path.size();) {
    size_t end = path.find('/', start);
    if (end == std::string::npos) end = path.size();
    const std::string name = path.substr(start, end - start);
    if (name == "..") {
      if (!names.empty()) names.pop_back();
    } else if (!name.empty() && name != ".") {
      names.push_back(name);
    }
    start = end + 1;
  }
  std::string resolved = path.substr(0, root);
  for (size_t i = 0; i < names.size(); ++i) {
    resolved += (i == 0 ? "" : "/") + names[i];
  }
  return resolved;
}

std::string trim(const std::string &value) {
  const size_t start = value.find_first_not_of(" \t\r\n");
  if (start == std::string::npos) return std::string();
  return value.substr(start, value.find_last_not_of(" \t\r\n") - start + 1);
}

std::string lower(std::string value) {
  for (char &c : value) c = lowerAscii(c);
  return value;
}

std::string homeDirectory() {
#if defined(_WIN32)
  // In UTF-8, not the ANSI code page getenv() uses.
  const wchar_t *home = _wgetenv(L"HOME");
  if (home == NULL || *home == L'\0') home = _wgetenv(L"USERPROFILE");
  return home == NULL ? std::string() : toGeneric(fromWide(home));
#else
  const char *home = getenv("HOME");
  return home == NULL ? std::string() : toGeneric(home);
#endif
}

std::string environment(const char *name) {
#if defined(_WIN32)
  const wchar_t *value = _wgetenv(toWide(name).c_str());
  return value == NULL ? std::string() : toGeneric(fromWide(value));
#else
  const char *value = getenv(name);
  return value == NULL ? std::string() : value;
#endif
}

std::string expandHome(const std::string &path) {
  const std::string home = homeDirectory();
  return path.compare(0, 2, "~/") == 0 && !home.empty() ? home + path.substr(1) : path;
}

std::string comparable(const std::string &path, bool caseSensitive) {
  std::string value = path;
  if (startsWith(value, "//?/UNC/", false)) {
    value = "//" + value.substr(8);
  } else if (startsWith(value, "//?/", true)) {
    value = value.substr(4);
  }
  return caseSensitive ? value : lower(value);
}

}
