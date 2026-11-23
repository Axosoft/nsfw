#include "../includes/GitignoreRules.h"

#include <cctype>
#include <cstdint>
#include <cstring>
#include <unordered_map>

namespace {

char lowerAscii(char c) {
  return c >= 'A' && c <= 'Z' ? static_cast<char>(c - 'A' + 'a') : c;
}

char upperAscii(char c) {
  return c >= 'a' && c <= 'z' ? static_cast<char>(c - 'a' + 'A') : c;
}

// Git's wildmatch() with WM_PATHNAME, ported from its algorithm: '*', '?' and classes don't match '/', and "**"
// between separators (or at either end) matches across them.
class Glob {
public:
  Glob(const char *pattern, bool caseSensitive):
    mPattern(pattern),
    mCaseSensitive(caseSensitive),
    mMemoized(strstr(pattern, "**") != NULL) {}

  bool matches(const char *text) const {
    mText = text;
    mMemo.clear();
    return match(mPattern, text) == MATCH;
  }

private:
  // As in wildmatch: ABORT_ALL once the text ran out, ABORT_TO_STARSTAR once a '/' stopped a '*', which no other
  // length of an earlier '*' gets past either. They keep a pattern with many '*' from backtracking exponentially.
  enum Result { MATCH, NO_MATCH, ABORT_ALL, ABORT_TO_STARSTAR };

  bool same(char a, char b) const {
    return mCaseSensitive ? a == b : lowerAscii(a) == lowerAscii(b);
  }

  bool inRange(char low, char high, char c) const {
    auto within = [low, high](char value) {
      return (unsigned char)low <= (unsigned char)value && (unsigned char)value <= (unsigned char)high;
    };
    return within(c) || (!mCaseSensitive && (within(lowerAscii(c)) || within(upperAscii(c))));
  }

  // Whether c is in the POSIX class name[0, length), false in found when there is no such class.
  bool inNamedClass(const char *name, size_t length, char c, bool &found) const {
    const std::string className(name, length);
    const int value = (unsigned char)c;
    found = true;
    if (className == "alnum") return isalnum(value) != 0;
    if (className == "alpha") return isalpha(value) != 0;
    if (className == "blank") return c == ' ' || c == '\t';
    if (className == "cntrl") return iscntrl(value) != 0;
    if (className == "digit") return isdigit(value) != 0;
    if (className == "graph") return isgraph(value) != 0;
    if (className == "lower") return islower(value) != 0 || (!mCaseSensitive && isupper(value) != 0);
    if (className == "print") return isprint(value) != 0;
    if (className == "punct") return ispunct(value) != 0;
    if (className == "space") return isspace(value) != 0;
    if (className == "upper") return isupper(value) != 0 || (!mCaseSensitive && islower(value) != 0);
    if (className == "xdigit") return isxdigit(value) != 0;
    found = false;
    return false;
  }

  // p is at the '['. Returns at its ']', or NULL for a class that never ends or names no POSIX class, which aborts
  // the whole match, as in git.
  const char *matchClass(const char *p, char c, bool &matched) const {
    char current = *++p;
    if (current == '^') current = '!';
    const bool negated = current == '!';
    if (negated) current = *++p;
    char previous = 0;
    bool found = false;
    do {
      if (current == '\0') {
        return NULL;
      }
      if (current == '\\') {
        current = *++p;
        if (current == '\0') return NULL;
        found = found || same(current, c);
      } else if (current == '-' && previous != 0 && p[1] != '\0' && p[1] != ']') {
        current = *++p;
        if (current == '\\') {
          current = *++p;
          if (current == '\0') return NULL;
        }
        found = found || inRange(previous, current, c);
        // A range doesn't start another one.
        current = 0;
      } else if (current == '[' && p[1] == ':') {
        const char *name = p + 2;
        const char *close = name;
        while (*close != '\0' && *close != ']') ++close;
        if (*close == '\0') return NULL;
        if (close - name < 1 || close[-1] != ':') {
          // No ":]": an ordinary '['.
          found = found || c == '[';
        } else {
          bool known;
          found = inNamedClass(name, close - name - 1, c, known) || found;
          if (!known) return NULL;
          p = close;
          current = 0;
        }
      } else {
        found = found || same(current, c);
      }
      previous = current;
      current = *++p;
    } while (current != ']');
    matched = found != negated;
    return p;
  }

  // Each "**" tries the rest of the pattern at every '/' left, and the next one again within each try: remembering what
  // each pattern and text position gave keeps a pattern with several of them from taking exponential time, as git's
  // own matcher can.
  Result match(const char *p, const char *t) const {
    if (!mMemoized) {
      return matchFrom(p, t);
    }
    const uint64_t key = ((uint64_t)(p - mPattern) << 32) | (uint64_t)(t - mText);
    auto found = mMemo.find(key);
    if (found != mMemo.end()) {
      return found->second;
    }
    const Result result = matchFrom(p, t);
    mMemo.emplace(key, result);
    return result;
  }

  Result matchFrom(const char *p, const char *t) const {
    for (; *p != '\0'; ++p, ++t) {
      const char c = *t;
      if (c == '\0' && *p != '*') {
        return ABORT_ALL;
      }
      switch (*p) {
        case '\\':
          ++p;
          if (*p == '\0' || !same(*p, c)) return NO_MATCH;
          break;
        case '?':
          if (c == '/') return NO_MATCH;
          break;
        case '[': {
          bool matched = false;
          const char *end = matchClass(p, c, matched);
          if (end == NULL) return ABORT_ALL;
          if (!matched || c == '/') return NO_MATCH;
          p = end;
          break;
        }
        case '*': {
          const char *stars = p;
          while (p[1] == '*') ++p;
          bool matchSlash = false;
          if (p > stars && (stars == mPattern || stars[-1] == '/')) {
            const char *next = p + 1;
            if (*next == '\0' || *next == '/' || (next[0] == '\\' && next[1] == '/')) {
              // "**/" also matches no directory at all.
              if (*next == '/' && match(next + 1, t) == MATCH) return MATCH;
              matchSlash = true;
            }
          }
          ++p;
          if (*p == '\0') {
            return matchSlash || strchr(t, '/') == NULL ? MATCH : NO_MATCH;
          }
          if (!matchSlash && *p == '/') {
            // A single '*' stops at the next '/', which the loop matches with this one.
            const char *slash = strchr(t, '/');
            if (slash == NULL) return NO_MATCH;
            t = slash;
            break;
          }
          for (; *t != '\0'; ++t) {
            const Result result = match(p, t);
            if (result != NO_MATCH) {
              if (!matchSlash || result != ABORT_TO_STARSTAR) return result;
            } else if (!matchSlash && *t == '/') {
              return ABORT_TO_STARSTAR;
            }
          }
          return ABORT_ALL;
        }
        default:
          if (!same(*p, c)) return NO_MATCH;
      }
    }
    return *t == '\0' ? MATCH : NO_MATCH;
  }

  const char *mPattern;
  bool mCaseSensitive;
  bool mMemoized;
  mutable const char *mText = NULL;
  mutable std::unordered_map<uint64_t, Result> mMemo;
};

}

bool globMatch(const std::string &pattern, const std::string &text, bool caseSensitive) {
  return Glob(pattern.c_str(), caseSensitive).matches(text.c_str());
}

namespace {

const size_t MAX_PATTERN_LENGTH = 1024;

}

// The rules of an ignore file, or of the excluded patterns, in their order.
GitignoreRules parseGitignoreRules(const std::string &content) {
  GitignoreRules rules;
  size_t start = content.compare(0, 3, "\xEF\xBB\xBF") == 0 ? 3 : 0;
  while (start < content.size()) {
    size_t end = content.find('\n', start);
    if (end == std::string::npos) {
      end = content.size();
    }
    std::string line = content.substr(start, end - start);
    start = end + 1;

    if (!line.empty() && line.back() == '\r') {
      line.pop_back();
    }
    if (line.empty() || line[0] == '#') {
      continue;
    }
    // Trailing spaces go unless escaped, as git trims them: escapes are read in pairs from the start, so "\\\\ " is an
    // escaped '\\' followed by a space that goes. The '\\' stays: the pattern matches the escaped space as it is.
    size_t keep = 0;
    for (size_t i = 0; i < line.size(); ++i) {
      if (line[i] == '\\' && i + 1 < line.size()) {
        keep = ++i + 1;
      } else if (line[i] != ' ') {
        keep = i + 1;
      }
    }
    line.resize(keep);

    GitignoreRule rule = { "", false, false, false };
    if (!line.empty() && line[0] == '!') {
      rule.negated = true;
      line.erase(0, 1);
    }
    if (!line.empty() && line.back() == '/') {
      rule.directoryOnly = true;
      line.pop_back();
    }
    rule.anchored = line.find('/') != std::string::npos;
    if (!line.empty() && line[0] == '/') {
      line.erase(0, 1);
    }
    // "**/**/" matches what "**/" does; collapsed, a chain of them can't nest the matcher that deep.
    for (size_t chain = line.find("**/**/"); chain != std::string::npos; chain = line.find("**/**/", chain)) {
      if (chain == 0 || line[chain - 1] == '/') {
        line.erase(chain, 3);
      } else {
        chain += 3;
      }
    }
    // Longer than any path it could match, and each '*' nests the matcher once more.
    if (line.empty() || line.size() > MAX_PATTERN_LENGTH) {
      continue;
    }
    rule.pattern = line;
    rules.push_back(rule);
  }
  return rules;
}

// The last rule that matches decides. relative: the path relative to the directory the rules are in.
GitignoreMatch matchGitignoreRules(const GitignoreRules &rules, const std::string &relative, bool isDirectory,
  bool caseSensitive) {
  const size_t slash = relative.rfind('/');
  const std::string name = slash == std::string::npos ? relative : relative.substr(slash + 1);
  for (auto rule = rules.rbegin(); rule != rules.rend(); ++rule) {
    if (rule->directoryOnly && !isDirectory) {
      continue;
    }
    if (globMatch(rule->pattern, rule->anchored ? relative : name, caseSensitive)) {
      return rule->negated ? GitignoreMatch::INCLUDED : GitignoreMatch::IGNORED;
    }
  }
  return GitignoreMatch::NONE;
}


