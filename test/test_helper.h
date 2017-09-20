#if defined(__linux__)
#include <sys/types.h>
#include <sys/stat.h>
#endif

#include <iostream>
#include <fstream>
#include <cstdio>

void makeDirectory(const std::string &path)
{
#if defined(_WIN32)
    _mkdir(path.c_str());
#elif defined(__linux__)
    mkdir(path.c_str(), 0777);
#endif
}

void makeFile(const std::string &path)
{
    std::ofstream(path.c_str());
}

void renameFile(const std::string &oldPath, const std::string &newPath)
{
    std::rename(oldPath.c_str(), newPath.c_str());
}

void modifyFile(const std::string &path)
{
    std::ofstream(path.c_str()).put('a');
}

void removeFile(const std::string &path)
{
    std::remove(path.c_str());
}

class DummyFile
{
    std::string path;
public:
    DummyFile(const std::string &path)
        : path(path) {
        std::ofstream(path.c_str());
    }

    void rename(const std::string &newPath)
    {
        std::rename(path.c_str(), newPath.c_str());
        path = newPath;
    }

    void modify(const std::string &payload)
    {
        std::ofstream ofs(path.c_str());
        ofs << payload;
    }

    ~DummyFile()
    {
        std::remove(path.c_str());
    }
};
