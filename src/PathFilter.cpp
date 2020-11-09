#include "../includes/PathFilter.h"

#pragma unmanaged

static const std::string doubleStarPrefix("**/");

static bool startsWith(const std::string &prefix, const std::string &str) {
  return str.length() >= prefix.length() && std::equal(prefix.begin(), prefix.end(), str.begin());
}

static const std::string ensureTrailingSeparator(const std::string &path) {
  if (path.back() == '/') {
    return path;
  } else {
    return path + '/';
  }
}

static const std::string normalizeTail(const std::string &path) {
  int length = path.length();
  if (length == 0) {
    return path;
  }
  // Find the first '*' and remove trailing elements
  auto istar = path.find('*');
  if (istar != std::string::npos) {
    return path.substr(0, istar);
  }
  // Path should end with exactly one '/' or none at all.
  if (path[length - 1] == '/') {
    if (length == 1) {
      return path;
    }
    else {
      int i = length - 2;
      while (i >= 0 && path[--i] == '/');
      return path.substr(0, i + 2);
    }
  }
  return path + '/';
}

static void pushUnique(std::vector<std::string> &vector, const std::string &element) {
  for (auto e : vector) {
    if (e == element) {
      return;
    }
  }
  vector.push_back(element);
}

PathFilter::PathFilter(const std::string &root) : mRoot(ensureTrailingSeparator(root)) {}

PathFilter::~PathFilter() {}

void PathFilter::addIgnoreGlob(const std::string &glob) {
  // `/path/parts[/][*]`
  if (startsWith("/", glob)) {
    pushUnique(mRootFilters, normalizeTail(glob));
  }
  // `./path/parts[/][*]`
  else if (startsWith("./", glob)) {
    pushUnique(mRootFilters, normalizeTail(mRoot + glob.substr(2)));
  }
  // `**/path/parts[/][*]`
  else if (startsWith(doubleStarPrefix, glob)) {
    pushUnique(mFilters, '/' + normalizeTail(glob.substr(doubleStarPrefix.length())));
  }
  // `*path/parts[/][*]`
  else if (startsWith("*", glob)) {
    pushUnique(mFilters, normalizeTail(glob.substr(1)));
  }
  // `path/parts[/][*]`
  else {
    pushUnique(mFilters, '/' + normalizeTail(glob));
  }
}

bool PathFilter::isIgnored(const std::string &path) {
  for (std::string filter : mRootFilters) {
    if (startsWith(filter, path)) {
      return true;
    }
  }
  for (std::string filter : mFilters) {
    if (path.find(filter) != std::string::npos) {
      return true;
    }
  }
  return false;
}
