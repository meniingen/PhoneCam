#pragma once
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <winsock2.h>
#include <windows.h>
#include <string>
#include <vector>
#include <functional>

using LogFn = std::function<void(const std::wstring&)>;

namespace proc
{
    struct Child
    {
        HANDLE process = nullptr;
        HANDLE in = nullptr;   // write end -> child's stdin
        HANDLE out = nullptr;  // read end  <- child's stdout
    };

    std::wstring ExeDir();
    std::wstring ToWide(const std::string& s);
    void InitJob();
    bool Spawn(const std::wstring& cmdline, const std::wstring& cwd, bool pipeIn, bool pipeOut, Child& c, bool mergeStderr = false);
    void Kill(Child& c);
    bool Alive(const Child& c);
    bool RunCapture(const std::wstring& cmdline, const std::wstring& cwd, std::string& output, DWORD timeoutMs = 20000);
    std::vector<std::string> ListDevices(const std::wstring& adbExe);
}
