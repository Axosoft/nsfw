// Times PathFilter's judgement of paths at increasing depths with respectGitignore, the rules already read. On Linux
// or macOS, with an empty scratch directory, which becomes a repository:
//   c++ -O2 -std=c++17 -o benchmark-gitignore-depth js/scripts/benchmark-gitignore-depth.cpp src/PathFilter.cpp
//     src/FilterPaths.cpp src/GitRepository.cpp src/GitignoreRules.cpp    (one line)
//   ./benchmark-gitignore-depth <scratch directory> [evaluations]
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <string>
#include <sys/stat.h>

#include "../../includes/PathFilter.h"

int main(int argc, char **argv) {
  if (argc < 2) {
    std::fprintf(stderr, "Usage: %s <scratch directory> [evaluations]\n", argv[0]);
    return 1;
  }
  const std::string root = argv[1];
  const int evaluations = argc > 2 ? std::atoi(argv[2]) : 10000;
  mkdir((root + "/.git").c_str(), 0755);
  std::ofstream(root + "/.git/HEAD") << "ref: refs/heads/main\n";
  std::ofstream(root + "/.gitignore") << "*.o\n";
  FilterOptions options;
  options.respectGitignore = true;
  PathFilter filter(root, options);
  if (!filter.isActive()) {
    std::fprintf(stderr, "%s isn't taken for a git repository\n", root.c_str());
    return 1;
  }
  for (int depth : { 1, 4, 8, 16, 32, 64 }) {
    std::string directory = root;
    for (int level = 1; level < depth; ++level) {
      directory += "/d" + std::to_string(level);
    }
    // Warm: the .gitignore of each directory read once.
    filter.isIgnored(directory, "file.c", PathFilter::Kind::FILE);
    const auto start = std::chrono::steady_clock::now();
    for (int i = 0; i < evaluations; ++i) {
      filter.isIgnored(directory, i % 2 ? "file.c" : "file.o", PathFilter::Kind::FILE);
    }
    const std::chrono::duration<double, std::milli> elapsed = std::chrono::steady_clock::now() - start;
    std::printf("depth=%d evaluations=%d ms=%.1f us_per_event=%.2f\n", depth, evaluations, elapsed.count(),
      elapsed.count() * 1000 / evaluations);
  }
  return 0;
}
