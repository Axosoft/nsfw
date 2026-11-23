#ifndef NSFW_FILTER_PATHS_H
#define NSFW_FILTER_PATHS_H

#include <string>

// The paths and files PathFilter and GitRepository work with: '/' separated ("generic") once read, native when used.
namespace filterpaths {

enum class EntryKind { UNKNOWN, FILE, DIRECTORY };
enum class ReadResult { READ, MISSING, FAILED };

bool startsWith(const std::string &value, const std::string &prefix, bool caseSensitive);
std::string toGeneric(std::string path);
std::string fromGeneric(std::string path);
#if defined(_WIN32)
std::wstring toWide(const std::string &value);
std::string fromWide(const std::wstring &value);
#endif
EntryKind lookUp(const std::string &path);
ReadResult readFile(const std::string &path, std::string &contents, bool ignoreFile = false);
size_t rootLength(const std::string &path);
std::string join(const std::string &directory, const std::string &name);
bool isAbsolute(const std::string &path);
std::string withoutTrailingSeparators(std::string path);
std::string parentOf(const std::string &path);
std::string resolve(const std::string &directory, const std::string &relative);
std::string trim(const std::string &value);
std::string lower(std::string value);
std::string homeDirectory();
// An environment variable in UTF-8, '/' separated, empty if unset.
std::string environment(const char *name);
// A '/' separated path with a leading "~/" for the home directory, as git spells one in its config.
std::string expandHome(const std::string &path);
// One spelling of a '/' separated path: without the "\\?\" prefix a native Windows path may have, and in lower case
// unless caseSensitive.
std::string comparable(const std::string &path, bool caseSensitive);

}

#endif
