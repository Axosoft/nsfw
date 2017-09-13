#include "../includes/win32_2/Controller.h"


static std::wstring convertMultiByteToWideChar(const std::string &multiByte)
{
    const int wlen = MultiByteToWideChar(CP_UTF8, 0, multiByte.data(), -1, 0, 0);

    if (wlen == 0) {
        return std::wstring();
    }

    std::wstring wideString;
    wideString.reserve(wlen);
    int failureToResolveUTF8 = MultiByteToWideChar(CP_UTF8, 0, multiByte.data(), -1, const_cast<wchar_t*>(wideString.data()), wlen);
    if (failureToResolveUTF8 == 0) {
        return std::wstring();
    }
    return wideString;
}

Controller::Controller(EventQueue &queue, const std::string &path)
{
    auto wideString = convertMultiByteToWideChar(path);
    mWatcher.reset(new Watcher(queue, nullptr, wideString));
}

Controller::~Controller()
{
}

std::string Controller::getError()
{
    return "";
}

bool Controller::hasErrored()
{
    return false;
}

bool Controller::isWatching()
{
    return mWatcher->isRunning();
}
