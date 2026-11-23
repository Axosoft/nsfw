#ifndef NSFW_GITIGNORE_RULES_H
#define NSFW_GITIGNORE_RULES_H

#include <string>
#include <vector>

// A line of an ignore file, or an excluded pattern, with .gitignore syntax.
struct GitignoreRule {
  std::string pattern;
  bool negated;
  bool directoryOnly;
  // Matches the path relative to the directory of its file, not only the last name.
  bool anchored;
  bool operator==(const GitignoreRule &other) const {
    return pattern == other.pattern && negated == other.negated && directoryOnly == other.directoryOnly &&
      anchored == other.anchored;
  }
};
using GitignoreRules = std::vector<GitignoreRule>;

GitignoreRules parseGitignoreRules(const std::string &content);

enum class GitignoreMatch { NONE, IGNORED, INCLUDED };
// The last rule that matches decides. relative: the path relative to the directory the rules are in.
GitignoreMatch matchGitignoreRules(const GitignoreRules &rules, const std::string &relative, bool isDirectory,
  bool caseSensitive);

// Git's wildmatch() with WM_PATHNAME.
bool globMatch(const std::string &pattern, const std::string &text, bool caseSensitive);

#endif
