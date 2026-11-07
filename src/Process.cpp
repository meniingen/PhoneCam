#include "Process.h"
#include <sstream>

namespace proc
{
    static HANDLE g_job = nullptr;

    std::wstring ExeDir()
    {
        wchar_t path[MAX_PATH];
        GetModuleFileNameW(nullptr, path, MAX_PATH);
        std::wstring s(path);
        return s.substr(0, s.find_last_of(L"\\/"));
    }

    std::wstring ToWide(const std::string& s)
    {
        if (s.empty()) return L"";
        int n = MultiByteToWideChar(CP_UTF8, 0, s.data(), (int)s.size(), nullptr, 0);
        std::wstring w(n, L'\0');
        MultiByteToWideChar(CP_UTF8, 0, s.data(), (int)s.size(), &w[0], n);
        return w;
    }

    void InitJob()
    {
        if (g_job) return;
        g_job = CreateJobObjectW(nullptr, nullptr);
        if (!g_job) return;
        JOBOBJECT_EXTENDED_LIMIT_INFORMATION info = {};
        info.BasicLimitInformation.LimitFlags = JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE;
        SetInformationJobObject(g_job, JobObjectExtendedLimitInformation, &info, sizeof(info));
    }

    bool Spawn(const std::wstring& cmdline, const std::wstring& cwd, bool pipeIn, bool pipeOut, Child& c, bool mergeStderr)
    {
        SECURITY_ATTRIBUTES sa = { sizeof(sa), nullptr, TRUE };
        HANDLE inR = nullptr, inW = nullptr, outR = nullptr, outW = nullptr;
        HANDLE nul = CreateFileW(L"NUL", GENERIC_READ | GENERIC_WRITE, FILE_SHARE_READ | FILE_SHARE_WRITE, &sa, OPEN_EXISTING, 0, nullptr);

        if (pipeIn)
        {
            CreatePipe(&inR, &inW, &sa, 1 << 20);
            SetHandleInformation(inW, HANDLE_FLAG_INHERIT, 0);
        }
        if (pipeOut)
        {
            CreatePipe(&outR, &outW, &sa, 1 << 22);
            SetHandleInformation(outR, HANDLE_FLAG_INHERIT, 0);
        }

        STARTUPINFOW si = {};
        si.cb = sizeof(si);
        si.dwFlags = STARTF_USESTDHANDLES;
        si.hStdInput = pipeIn ? inR : nul;
        si.hStdOutput = pipeOut ? outW : nul;
        si.hStdError = pipeOut && mergeStderr ? outW : nul;

        PROCESS_INFORMATION pi = {};
        std::wstring cmd = cmdline;
        BOOL ok = CreateProcessW(nullptr, &cmd[0], nullptr, nullptr, TRUE, CREATE_NO_WINDOW,
                                 nullptr, cwd.empty() ? nullptr : cwd.c_str(), &si, &pi);

        if (inR) CloseHandle(inR);
        if (outW) CloseHandle(outW);
        CloseHandle(nul);

        if (!ok)
        {
            if (inW) CloseHandle(inW);
            if (outR) CloseHandle(outR);
            return false;
        }
        if (g_job) AssignProcessToJobObject(g_job, pi.hProcess);
        CloseHandle(pi.hThread);
        c.process = pi.hProcess;
        c.in = inW;
        c.out = outR;
        return true;
    }

    void Kill(Child& c)
    {
        if (c.process)
        {
            TerminateProcess(c.process, 0);
            WaitForSingleObject(c.process, 2000);
            CloseHandle(c.process);
        }
        if (c.in) CloseHandle(c.in);
        if (c.out) CloseHandle(c.out);
        c = Child();
    }

    bool Alive(const Child& c)
    {
        return c.process && WaitForSingleObject(c.process, 0) == WAIT_TIMEOUT;
    }

    bool RunCapture(const std::wstring& cmdline, const std::wstring& cwd, std::string& output, DWORD timeoutMs)
    {
        SECURITY_ATTRIBUTES sa = { sizeof(sa), nullptr, TRUE };
        HANDLE r = nullptr, w = nullptr;
        CreatePipe(&r, &w, &sa, 0);
        SetHandleInformation(r, HANDLE_FLAG_INHERIT, 0);

        STARTUPINFOW si = {};
        si.cb = sizeof(si);
        si.dwFlags = STARTF_USESTDHANDLES;
        si.hStdInput = nullptr;
        si.hStdOutput = w;
        si.hStdError = w;

        PROCESS_INFORMATION pi = {};
        std::wstring cmd = cmdline;
        BOOL ok = CreateProcessW(nullptr, &cmd[0], nullptr, nullptr, TRUE, CREATE_NO_WINDOW,
                                 nullptr, cwd.empty() ? nullptr : cwd.c_str(), &si, &pi);
        CloseHandle(w);
        if (!ok) { CloseHandle(r); return false; }

        output.clear();
        char buf[4096];
        DWORD n;
        while (ReadFile(r, buf, sizeof(buf), &n, nullptr) && n > 0)
            output.append(buf, n);

        DWORD wr = WaitForSingleObject(pi.hProcess, timeoutMs);
        DWORD code = 1;
        if (wr == WAIT_TIMEOUT) TerminateProcess(pi.hProcess, 1);
        else GetExitCodeProcess(pi.hProcess, &code);
        CloseHandle(pi.hProcess);
        CloseHandle(pi.hThread);
        CloseHandle(r);
        return wr != WAIT_TIMEOUT && code == 0;
    }

    std::vector<std::string> ListDevices(const std::wstring& adbExe)
    {
        std::vector<std::string> list;
        std::string out;
        if (!RunCapture(L"\"" + adbExe + L"\" devices", L"", out)) return list;
        std::istringstream ss(out);
        std::string line;
        while (std::getline(ss, line))
        {
            if (!line.empty() && line.back() == '\r') line.pop_back();
            size_t tab = line.find('\t');
            if (tab != std::string::npos && line.substr(tab + 1) == "device")
                list.push_back(line.substr(0, tab));
        }
        return list;
    }
}
