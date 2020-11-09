#ifndef FILTER_H
#define FILTER_H

#include <string>
#include <vector>

class PathFilter {
    private:
        std::string mRoot;
        std::vector<std::string> mRootFilters;
        std::vector<std::string> mFilters;

    public:
        PathFilter(const std::string &root);
        ~PathFilter();

        /** register a glob filter */
        void addIgnoreGlob(const std::string &glob);
        /** return true if path should be ignored, false otherwise */
        bool isIgnored(const std::string &path);
};

#endif
