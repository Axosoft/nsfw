#ifndef NSFW_GIT_REPOSITORY_H
#define NSFW_GIT_REPOSITORY_H

#include <string>

// '/' separated paths; the ignore files' are empty when there is none to read.
struct GitRepository {
  std::string root;
  std::string infoExcludePath;
  std::string excludesFilePath;
};

// Whether the watched path is in a git repository the user trusts, as git decides: owned by them, or by the user who
// ran sudo, or the Administrators group they are in on Windows, or listed in safe.directory; on the same filesystem;
// with no path read from its files leading to another machine.
bool findGitRepository(const std::string &watchedPath, GitRepository &repository);

#endif
