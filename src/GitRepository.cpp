#include "../includes/GitRepository.h"

#include "../includes/FilterPaths.h"

#include <cctype>
#include <climits>
#include <cstdlib>
#include <vector>
#include <sys/stat.h>

#if defined(_WIN32)
#include <Windows.h>
#include <aclapi.h>
#else
#include <unistd.h>
#endif

using namespace filterpaths;

namespace {

// A value as git's parse_value() reads it from pos, just past its '=', up to the end of its line, pos left past it.
// Quotes may open and close anywhere, a backslash escapes '\\', '"', a newline, which continues the value on the next
// line, and stands for a tab, a newline or a backspace before t, n and b; '#' or ';' outside quotes starts a comment, in
// which a backslash is no escape. Surrounding whitespace goes, the one inside stays.
// A CRLF, and the end of the file, read as a newline. False for one git refuses (an unterminated quote, an unknown
// escape), whose config it then doesn't read.
bool parseValue(const std::string &contents, size_t &pos, std::string &value) {
  auto next = [&contents, &pos]() {
    if (pos >= contents.size()) return '\n';
    const char c = contents[pos++];
    if (c == '\r' && pos < contents.size() && contents[pos] == '\n') {
      ++pos;
      return '\n';
    }
    return c;
  };
  value.clear();
  bool quoted = false;
  bool comment = false;
  std::string spaces;
  for (;;) {
    char c = next();
    if (c == '\n') {
      return !quoted;
    }
    if (comment) {
      continue;
    }
    if (!quoted && (c == ' ' || c == '\t' || c == '\r' || c == '\v' || c == '\f')) {
      if (!value.empty()) spaces += c;
      continue;
    }
    if (!quoted && (c == '#' || c == ';')) {
      comment = true;
      continue;
    }
    value += spaces;
    spaces.clear();
    if (c == '\\') {
      c = next();
      if (c == '\n') {
        continue;
      }
      if (c != '\\' && c != '"' && c != 't' && c != 'n' && c != 'b') {
        return false;
      }
      value += c == 't' ? '\t' : c == 'n' ? '\n' : c == 'b' ? '\b' : c;
      continue;
    }
    if (c == '"') {
      quoted = !quoted;
      continue;
    }
    value += c;
  }
}

// The values a git config file gives the key in section, in their order. Only what that takes is read: sections, keys
// and values, quoted or not; includes aren't followed. Nothing from a file git refuses (a bad value or header).
std::vector<std::string> readConfigValues(const std::string &configPath, const std::string &wantedSection,
  const std::string &wantedKey) {
  std::vector<std::string> values;
  std::string contents;
  if (readFile(fromGeneric(configPath), contents) != ReadResult::READ) {
    return values;
  }
  // A byte order mark first, which git skips too.
  if (contents.compare(0, 3, "\xEF\xBB\xBF") == 0) {
    contents.erase(0, 3);
  }
  auto lineEnd = [&contents](size_t from) {
    const size_t end = contents.find('\n', from);
    return end == std::string::npos ? contents.size() : end;
  };
  std::string section;
  size_t pos = 0;
  for (;;) {
    pos = contents.find_first_not_of(" \t\r\n", pos);
    if (pos == std::string::npos) {
      return values;
    }
    const size_t end = lineEnd(pos);
    if (contents[pos] == '#' || contents[pos] == ';') {
      pos = end;
      continue;
    }
    if (contents[pos] == '[') {
      const size_t close = contents.find(']', pos);
      if (close == std::string::npos || close > end) {
        return {};
      }
      const std::string header = trim(contents.substr(pos + 1, close - pos - 1));
      // A subsection, [core "name"], is another section than [core].
      section = header.find_first_of(" \t\"") == std::string::npos ? lower(header) : "";
      // A key can follow on the same line, as git reads it.
      pos = close + 1;
      continue;
    }
    const size_t equals = contents.find('=', pos);
    if (equals == std::string::npos || equals > end) {
      // A key alone: true.
      pos = end;
      continue;
    }
    const std::string key = lower(trim(contents.substr(pos, equals - pos)));
    // A name of letters, digits and '-', starting with a letter, or git refuses the file.
    if (key.empty() || !isalpha((unsigned char)key[0]) ||
      key.find_first_not_of("abcdefghijklmnopqrstuvwxyz0123456789-") != std::string::npos) {
      return {};
    }
    pos = equals + 1;
    std::string value;
    if (!parseValue(contents, pos, value)) {
      return {};
    }
    if (section == wantedSection && key == wantedKey) {
      values.push_back(value);
    }
  }
}

#if defined(_WIN32)
// The watched path comes namespaced and the filesystem ignores case, while git writes C:/repo.
const bool CASE_SENSITIVE = false;
#else
const bool CASE_SENSITIVE = true;
#endif

// Owned by the user running this, as git tells it: under sudo, the user who ran sudo; on Windows, by the user's SID,
// or by the Administrators group the user is a member of.
bool isOwnedByCurrentUser(const std::string &path) {
#if defined(_WIN32)
  PSID owner = NULL;
  PSECURITY_DESCRIPTOR descriptor = NULL;
  if (GetNamedSecurityInfoW(toWide(fromGeneric(path)).c_str(), SE_FILE_OBJECT, OWNER_SECURITY_INFORMATION, &owner,
    NULL, NULL, NULL, &descriptor) != ERROR_SUCCESS) {
    return false;
  }
  bool owned = false;
  HANDLE token = NULL;
  if (owner != NULL && OpenProcessToken(GetCurrentProcess(), TOKEN_QUERY, &token)) {
    DWORD size = 0;
    GetTokenInformation(token, TokenUser, NULL, 0, &size);
    std::vector<BYTE> user(size);
    if (size > 0 && GetTokenInformation(token, TokenUser, user.data(), size, &size)) {
      owned = EqualSid(owner, reinterpret_cast<TOKEN_USER *>(user.data())->User.Sid) != FALSE;
    }
    BOOL member = FALSE;
    if (!owned && IsWellKnownSid(owner, WinBuiltinAdministratorsSid) && CheckTokenMembership(NULL, owner, &member)) {
      owned = member != FALSE;
    }
    CloseHandle(token);
  }
  LocalFree(descriptor);
  return owned;
#else
  struct stat status;
  if (lstat(path.c_str(), &status) != 0) {
    return false;
  }
  uid_t user = geteuid();
  const char *sudoUser = getenv("SUDO_UID");
  if (user == 0 && sudoUser != NULL && *sudoUser != '\0') {
    char *end = NULL;
    const unsigned long parsed = strtoul(sudoUser, &end, 10);
    if (end != NULL && *end == '\0') {
      user = (uid_t)parsed;
    }
  }
  return status.st_uid == user;
#endif
}

// A safe.directory value as git compares it: by its real path, which the watched path is too. "*" and a trailing "/*"
// stay.
std::string canonical(const std::string &value) {
#if defined(_WIN32)
  return value;
#else
  const bool prefix = value.size() >= 2 && value.compare(value.size() - 2, 2, "/*") == 0;
  const std::string path = prefix ? value.substr(0, value.size() - 2) : value;
  char resolved[PATH_MAX];
  if (value == "*" || path.empty() || realpath(path.c_str(), resolved) == NULL) {
    return value;
  }
  return std::string(resolved) + (prefix ? (resolved[1] == '\0' ? "*" : "/*") : "");
#endif
}

// Whether safe.directory, in the system or the user's config as git reads it, trusts the repository at root: "*", the
// path itself, or a path ending in "/*" it is below. An empty value forgets the ones before it.
bool isSafeDirectory(const std::string &root, const std::vector<std::string> &configs) {
  std::vector<std::string> safe;
  for (const std::string &config : configs) {
    for (const std::string &value : readConfigValues(config, "safe", "directory")) {
      const std::string path = expandHome(toGeneric(value));
      if (value.empty()) {
        safe.clear();
      } else if (value == "*" || isAbsolute(path) || path[0] == '/') {
        // git skips a relative one, which would depend on where it runs.
        safe.push_back(canonical(path));
      }
    }
  }
  const std::string comparableRoot = comparable(root, CASE_SENSITIVE);
  for (const std::string &value : safe) {
    const std::string path = comparable(value, CASE_SENSITIVE);
    if (value == "*" || withoutTrailingSeparators(path) == comparableRoot ||
      (path.size() >= 2 && path.compare(path.size() - 2, 2, "/*") == 0 &&
        comparableRoot.compare(0, path.size() - 1, path, 0, path.size() - 1) == 0)) {
      return true;
    }
  }
  return false;
}

// Whether a path read from a repository's files stays on this machine: on Windows a UNC path would have the lookup
// authenticate to another one, unless the repository itself is on that share.
bool isLocal(const std::string &path, const std::string &root) {
#if defined(_WIN32)
  // A drive, or a volume by its GUID; any other device path (\\.\, \\?\GLOBALROOT) can reach another machine.
  const bool namespaced = startsWith(path, "//?/", true);
  const size_t drive = namespaced ? 4 : 0;
  if ((!startsWith(path, "//", true) && path.size() > 1 && path[1] == ':') ||
    (namespaced && path.size() > drive + 1 && path[drive + 1] == ':' && isalpha((unsigned char)path[drive])) ||
    startsWith(path, "//?/Volume{", false)) {
    return true;
  }
  if (!startsWith(path, "//?/UNC/", false) && (startsWith(path, "//?/", true) || startsWith(path, "//./", true))) {
    return false;
  }
  const size_t length = rootLength(path);
  return length > 0 && rootLength(root) == length && startsWith(root, path.substr(0, length), false);
#else
  (void)path;
  (void)root;
  return true;
#endif
}


}

// The closest directory with a .git (a directory, or a file pointing to one, as in a worktree or a submodule) at or
// above the watched path, if the user can trust it as git does, with the ignore files that don't change with the paths
// below it.
bool findGitRepository(const std::string &watchedPath, GitRepository &repository) {
  std::string root = watchedPath;
  EntryKind dotGit = EntryKind::UNKNOWN;
#if !defined(_WIN32)
  // As git, which doesn't look for a repository on another filesystem than the one it starts from.
  struct stat start;
  const bool startKnown = stat(watchedPath.c_str(), &start) == 0;
#endif
  for (; !root.empty(); root = parentOf(root)) {
#if !defined(_WIN32)
    struct stat directory;
    if (startKnown && stat(root.c_str(), &directory) == 0 && directory.st_dev != start.st_dev) {
      return false;
    }
#endif
    dotGit = lookUp(fromGeneric(join(root, ".git")));
    if (dotGit != EntryKind::UNKNOWN) {
      break;
    }
  }
  if (root.empty()) {
    return false;
  }
  // As git's safe.directory check: a repository someone else owns, a /tmp/.git or a C:\.git above the watched path
  // say, doesn't get to decide what is ignored.
  const std::string home = homeDirectory();
  const std::string xdg = environment("XDG_CONFIG_HOME");
  const std::string configHome = !xdg.empty() ? xdg : (home.empty() ? "" : home + "/.config");
  std::vector<std::string> globalConfigs;
#if !defined(_WIN32)
  globalConfigs.push_back("/etc/gitconfig");
#endif
  if (!configHome.empty()) globalConfigs.push_back(configHome + "/git/config");
  if (!home.empty()) globalConfigs.push_back(home + "/.gitconfig");
  const bool safe = isSafeDirectory(root, globalConfigs);
  auto isTrusted = [safe](const std::string &path) { return safe || isOwnedByCurrentUser(path); };
  if (!isTrusted(root) || !isTrusted(join(root, ".git"))) {
    return false;
  }

  std::string gitDirectory = join(root, ".git");
  if (dotGit == EntryKind::FILE) {
    std::string contents;
    readFile(fromGeneric(gitDirectory), contents);
    contents = trim(contents);
    gitDirectory = contents.compare(0, 7, "gitdir:") == 0 ? resolve(root, toGeneric(trim(contents.substr(7)))) : "";
    if (!gitDirectory.empty() && (!isLocal(gitDirectory, root) || !isTrusted(gitDirectory))) {
      return false;
    }
  }
  std::string commonDirectory = gitDirectory;
  std::string common;
  if (!gitDirectory.empty() && readFile(fromGeneric(gitDirectory + "/commondir"), common) == ReadResult::READ &&
    !trim(common).empty()) {
    commonDirectory = resolve(gitDirectory, toGeneric(trim(common)));
    if (!isLocal(commonDirectory, root)) {
      return false;
    }
  }
  repository.root = root;
  repository.infoExcludePath = commonDirectory.empty() ? "" : commonDirectory + "/info/exclude";

  // As git reads them, the later ones winning: the system and user config files, then the repository's.
  std::vector<std::string> configs = globalConfigs;
  if (!commonDirectory.empty()) configs.push_back(commonDirectory + "/config");
  std::string excludesFile = configHome.empty() ? "" : configHome + "/git/ignore";
  for (const std::string &config : configs) {
    const std::vector<std::string> settings = readConfigValues(config, "core", "excludesfile");
    if (!settings.empty()) {
      std::string setting = expandHome(toGeneric(settings.back()));
      // The repository's own config can't send a lookup to another machine.
      const bool fromRepository = &config == &configs.back() && !commonDirectory.empty();
      excludesFile = isAbsolute(setting) && (!fromRepository || isLocal(setting, root)) ? setting : "";
    }
  }
  repository.excludesFilePath = excludesFile;
  return true;
}
